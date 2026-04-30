"""
v4l2_helper.py — V4L2 MMAP + dma_buf 导出封装（多平面版）
=========================================================
使用 ctypes + fcntl.ioctl 直接操作 V4L2 驱动，绕过 OpenCV cap.read() 的用户空间拷贝。

Rockchip rkcif/rkisp 驱动使用 Video Capture Multiplanar API（BUF_TYPE=9），
本文件使用 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 替代单平面 BUF_TYPE=1。

核心流程：
    open(/dev/video-camera0)
        → VIDIOC_S_FMT        设置格式（NV12, 1280x720, MPLANE）
        → VIDIOC_REQBUFS       申请 N 个 MMAP buffer
        → VIDIOC_QUERYBUF      查询每个 buffer 的 plane offset/size
        → mmap()               把 plane 0 映射到用户空间（调试用）
        → VIDIOC_EXPBUF        导出 plane 0 的 dma_buf fd（★零拷贝关键）
        → VIDIOC_QBUF          把所有 buffer 入队
        → VIDIOC_STREAMON      开始采集

    每帧：
        → VIDIOC_DQBUF         取出一帧（拿到 buffer 索引）
        → 返回该 buffer 的 dma_buf fd（直接给 RGA 用）
        → VIDIOC_QBUF          把 buffer 还回队列
"""

import os
import mmap
import ctypes
import fcntl
import errno as errno_mod
import logging

log = logging.getLogger(__name__)

# ──────────────────────────────────────────────────────────────
# V4L2 常量 (来自 linux/videodev2.h)
# ──────────────────────────────────────────────────────────────

_IOC_NONE  = 0
_IOC_WRITE = 1
_IOC_READ  = 2

def _IOC(dir_, type_, nr, size):
    return (dir_ << 30) | (size << 16) | (ord(type_) << 8) | nr

def _IOWR(type_, nr, size): return _IOC(_IOC_READ | _IOC_WRITE, type_, nr, size)
def _IOW(type_, nr, size):  return _IOC(_IOC_WRITE, type_, nr, size)
def _IOR(type_, nr, size):  return _IOC(_IOC_READ,  type_, nr, size)
def _IO(type_, nr):         return _IOC(_IOC_NONE,  type_, nr, 0)

def v4l2_fourcc(a, b, c, d):
    return ord(a) | (ord(b) << 8) | (ord(c) << 16) | (ord(d) << 24)

V4L2_PIX_FMT_NV12  = v4l2_fourcc('N', 'V', '1', '2')
V4L2_PIX_FMT_BGR24 = v4l2_fourcc('B', 'G', 'R', '3')
V4L2_PIX_FMT_RGB24 = v4l2_fourcc('R', 'G', 'B', '3')

# Rockchip rkcif/rkisp 使用多平面 API
V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE = 9
V4L2_MEMORY_MMAP   = 1
V4L2_FIELD_NONE    = 1
VIDEO_MAX_PLANES   = 8

# ──────────────────────────────────────────────────────────────
# V4L2 结构体定义（多平面版）
# ──────────────────────────────────────────────────────────────

class v4l2_plane_pix_format(ctypes.Structure):
    """单个平面的格式描述（packed，来自 videodev2.h）"""
    _pack_ = 1
    _fields_ = [
        ('sizeimage',    ctypes.c_uint32),
        ('bytesperline', ctypes.c_uint32),
        ('reserved',     ctypes.c_uint16 * 6),
    ]

class v4l2_pix_format_mplane(ctypes.Structure):
    """多平面像素格式（packed）"""
    _pack_ = 1
    _fields_ = [
        ('width',        ctypes.c_uint32),
        ('height',       ctypes.c_uint32),
        ('pixelformat',  ctypes.c_uint32),
        ('field',        ctypes.c_uint32),
        ('colorspace',   ctypes.c_uint32),
        ('plane_fmt',    v4l2_plane_pix_format * VIDEO_MAX_PLANES),
        ('num_planes',   ctypes.c_uint8),
        ('flags',        ctypes.c_uint8),
        ('ycbcr_enc',    ctypes.c_uint8),
        ('quantization', ctypes.c_uint8),
        ('xfer_func',    ctypes.c_uint8),
        ('reserved',     ctypes.c_uint8 * 7),
    ]

class _v4l2_fmt_union(ctypes.Union):
    _fields_ = [
        ('pix_mp', v4l2_pix_format_mplane),
        ('raw',    ctypes.c_uint8 * 200),
    ]

class v4l2_format(ctypes.Structure):
    _fields_ = [
        ('type', ctypes.c_uint32),
        ('_u',   _v4l2_fmt_union),
    ]

class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [
        ('count',    ctypes.c_uint32),
        ('type',     ctypes.c_uint32),
        ('memory',   ctypes.c_uint32),
        ('reserved', ctypes.c_uint32 * 2),
    ]

class v4l2_timecode(ctypes.Structure):
    _fields_ = [
        ('type',     ctypes.c_uint32),
        ('flags',    ctypes.c_uint32),
        ('frames',   ctypes.c_uint8),
        ('seconds',  ctypes.c_uint8),
        ('minutes',  ctypes.c_uint8),
        ('hours',    ctypes.c_uint8),
        ('userbits', ctypes.c_uint8 * 4),
    ]

class v4l2_timeval(ctypes.Structure):
    _fields_ = [('tv_sec', ctypes.c_long), ('tv_usec', ctypes.c_long)]

# v4l2_plane — 多平面 buffer 中每个平面的描述
class _v4l2_plane_m(ctypes.Union):
    _fields_ = [
        ('mem_offset', ctypes.c_uint32),
        ('userptr',    ctypes.c_ulong),
        ('fd',         ctypes.c_int32),
    ]

class v4l2_plane(ctypes.Structure):
    _fields_ = [
        ('bytesused',  ctypes.c_uint32),
        ('length',     ctypes.c_uint32),
        ('m',          _v4l2_plane_m),
        ('data_offset', ctypes.c_uint32),
        ('reserved',   ctypes.c_uint32 * 11),
    ]

# v4l2_buffer — 多平面版，m.planes 是指向 v4l2_plane 数组的指针
class _v4l2_buf_m(ctypes.Union):
    _fields_ = [
        ('offset',  ctypes.c_uint32),
        ('userptr', ctypes.c_ulong),
        ('planes',  ctypes.POINTER(v4l2_plane)),
        ('fd',      ctypes.c_int32),
    ]

class v4l2_buffer(ctypes.Structure):
    _fields_ = [
        ('index',     ctypes.c_uint32),
        ('type',      ctypes.c_uint32),
        ('bytesused', ctypes.c_uint32),
        ('flags',     ctypes.c_uint32),
        ('field',     ctypes.c_uint32),
        ('timestamp', v4l2_timeval),
        ('timecode',  v4l2_timecode),
        ('sequence',  ctypes.c_uint32),
        ('memory',    ctypes.c_uint32),
        ('m',         _v4l2_buf_m),
        ('length',    ctypes.c_uint32),
        ('reserved2', ctypes.c_uint32),
        ('reserved',  ctypes.c_uint32),
    ]

class v4l2_exportbuffer(ctypes.Structure):
    _fields_ = [
        ('type',     ctypes.c_uint32),
        ('index',    ctypes.c_uint32),
        ('plane',    ctypes.c_uint32),
        ('flags',    ctypes.c_uint32),
        ('fd',       ctypes.c_int32),
        ('reserved', ctypes.c_uint32 * 11),
    ]

# ioctl 编号（基于结构体大小）
_VIDIOC_S_FMT     = _IOWR('V', 5,  ctypes.sizeof(v4l2_format))
_VIDIOC_G_FMT     = _IOWR('V', 4,  ctypes.sizeof(v4l2_format))
_VIDIOC_REQBUFS   = _IOWR('V', 8,  ctypes.sizeof(v4l2_requestbuffers))
_VIDIOC_QUERYBUF  = _IOWR('V', 9,  ctypes.sizeof(v4l2_buffer))
_VIDIOC_QBUF      = _IOWR('V', 15, ctypes.sizeof(v4l2_buffer))
_VIDIOC_DQBUF     = _IOWR('V', 17, ctypes.sizeof(v4l2_buffer))
_VIDIOC_EXPBUF    = _IOWR('V', 16, ctypes.sizeof(v4l2_exportbuffer))
_VIDIOC_STREAMON  = _IOW ('V', 18, ctypes.sizeof(ctypes.c_int))
_VIDIOC_STREAMOFF = _IOW ('V', 19, ctypes.sizeof(ctypes.c_int))

# ──────────────────────────────────────────────────────────────
# 异常类
# ──────────────────────────────────────────────────────────────

class V4L2Error(Exception):
    def __init__(self, ioctl_name: str, err: int):
        self.ioctl_name = ioctl_name
        self.err = err
        msg = f"V4L2 {ioctl_name} 失败: errno={err} ({os.strerror(err)})"
        hints = {
            errno_mod.EINVAL: "参数无效（格式/分辨率/buffer数量不被驱动支持？）",
            errno_mod.EBUSY:  "设备忙（是否有其他进程占用摄像头？）",
            errno_mod.ENOMEM: "内核内存不足（CMA不够？）",
            errno_mod.ENODEV: "设备不存在",
            errno_mod.EACCES: "权限不足",
        }
        if err in hints:
            msg += f"\n  提示: {hints[err]}"
        super().__init__(msg)


def _ioctl(fd: int, request: int, arg, name: str):
    try:
        fcntl.ioctl(fd, request, arg)
    except OSError as e:
        raise V4L2Error(name, e.errno) from e


# ──────────────────────────────────────────────────────────────
# 主类
# ──────────────────────────────────────────────────────────────

class V4L2Capture:
    """
    V4L2 多平面 MMAP 摄像头采集，支持 dma_buf fd 导出。

    用法：
        cam = V4L2Capture('/dev/video-camera0', 1280, 720, n_bufs=4)
        cam.open()
        dma_fd, buf_idx = cam.wait_frame()
        # 使用 dma_fd 传给 RGA
        cam.requeue(buf_idx)
        cam.close()
    """

    def __init__(self, device: str, width: int, height: int,
                 pixfmt: int = V4L2_PIX_FMT_NV12, n_bufs: int = 4):
        self.device  = device
        self.width   = width
        self.height  = height
        self.pixfmt  = pixfmt
        self.n_bufs  = n_bufs

        self._fd        = -1
        self._bufs      = []   # list of {'mmap': mmap_obj, 'size': int, 'dma_fd': int, 'offset': int}
        self._streaming = False
        self._num_planes = 1   # 由 S_FMT 响应更新

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *_):
        self.close()

    def open(self):
        log.info(f"[V4L2] 打开设备: {self.device}")
        self._fd = os.open(self.device, os.O_RDWR | os.O_NONBLOCK)
        log.debug(f"[V4L2] fd={self._fd}")

        self._set_format()
        self._request_buffers()
        self._query_and_export_buffers()
        self._queue_all_buffers()
        self._streamon()

    def close(self):
        if self._streaming:
            self._streamoff()
        for b in self._bufs:
            if b.get('dma_fd', -1) >= 0:
                os.close(b['dma_fd'])
            if b.get('mmap') is not None:
                try:
                    b['mmap'].close()
                except Exception:
                    pass
        self._bufs.clear()
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1
        log.info("[V4L2] 设备已关闭")

    # ── 格式设置 ──

    def _set_format(self):
        # 先尝试 S_FMT。Rockchip rkaiq_3A_server 运行时会锁定 ISP 管线，
        # G_FMT 和 S_FMT 均返回 EINVAL，但 REQBUFS/EXPBUF/STREAMON 仍可用。
        # 失败时记录警告并跳过，frame_size / num_planes 由 QUERYBUF 后填充。
        fmt = v4l2_format()
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        pix = fmt._u.pix_mp
        pix.width       = self.width
        pix.height      = self.height
        pix.pixelformat = self.pixfmt
        pix.field       = V4L2_FIELD_NONE
        pix.num_planes  = 1

        try:
            _ioctl(self._fd, _VIDIOC_S_FMT, fmt, 'VIDIOC_S_FMT')
            actual_w      = fmt._u.pix_mp.width
            actual_h      = fmt._u.pix_mp.height
            actual_fmt    = fmt._u.pix_mp.pixelformat
            actual_planes = fmt._u.pix_mp.num_planes
            actual_size   = fmt._u.pix_mp.plane_fmt[0].sizeimage
            log.info(f"[V4L2] 格式已设置: {actual_w}x{actual_h} "
                     f"fourcc=0x{actual_fmt:08x} num_planes={actual_planes} "
                     f"plane0_size={actual_size}字节")
        except V4L2Error as e:
            # rkaiq 锁定时 S_FMT/G_FMT 均返回 EINVAL，跳过并使用请求值
            log.warning(f"[V4L2] S_FMT 失败（rkaiq 可能锁定了管线），跳过: {e}")
            actual_w      = self.width
            actual_h      = self.height
            actual_planes = 1
            actual_size   = 0   # 由 QUERYBUF 填充

        if actual_w != self.width or actual_h != self.height:
            log.warning(f"[V4L2] 驱动修改了分辨率: {self.width}x{self.height} "
                        f"→ {actual_w}x{actual_h}")

        self._frame_size   = actual_size
        self._num_planes   = actual_planes
        self.actual_width  = actual_w
        self.actual_height = actual_h

    # ── Buffer 申请与导出 ──

    def _request_buffers(self):
        req = v4l2_requestbuffers()
        req.count  = self.n_bufs
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        req.memory = V4L2_MEMORY_MMAP
        _ioctl(self._fd, _VIDIOC_REQBUFS, req, 'VIDIOC_REQBUFS')
        actual_count = req.count
        log.info(f"[V4L2] 申请到 {actual_count} 个 buffer（请求 {self.n_bufs} 个）")
        if actual_count == 0:
            raise V4L2Error('VIDIOC_REQBUFS', errno_mod.ENOMEM)
        self.n_bufs = actual_count

    def _query_and_export_buffers(self):
        for i in range(self.n_bufs):
            # 为每个 buffer 分配 planes 数组
            planes = (v4l2_plane * VIDEO_MAX_PLANES)()

            buf = v4l2_buffer()
            buf.index   = i
            buf.type    = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
            buf.memory  = V4L2_MEMORY_MMAP
            buf.length  = self._num_planes          # 平面数
            buf.m.planes = planes                   # 指向 planes 数组
            _ioctl(self._fd, _VIDIOC_QUERYBUF, buf, f'VIDIOC_QUERYBUF[{i}]')

            # 使用 plane 0 的 offset 和 length（Y+UV 连续存储）
            offset = planes[0].m.mem_offset
            size   = planes[0].length
            # S_FMT 被 rkaiq 锁定时 _frame_size=0，用第一个 buf 的 plane 大小补充
            if i == 0 and self._frame_size == 0:
                self._frame_size = size
                log.info(f"[V4L2] frame_size 从 QUERYBUF 获取: {size}字节")
            log.debug(f"[V4L2] buf[{i}] plane0: offset=0x{offset:x} size={size}")

            # mmap（调试/显示用）
            try:
                mm = mmap.mmap(self._fd, size, mmap.MAP_SHARED,
                               mmap.PROT_READ, offset=offset)
            except OSError as e:
                log.warning(f"[V4L2] buf[{i}] mmap 失败（忽略）: {e}")
                mm = None

            # EXPBUF — 导出 plane 0 的 dma_buf fd
            exp = v4l2_exportbuffer()
            exp.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
            exp.index = i
            exp.plane = 0
            exp.flags = 0x80000  # O_CLOEXEC
            _ioctl(self._fd, _VIDIOC_EXPBUF, exp, f'VIDIOC_EXPBUF[{i}]')
            dma_fd = exp.fd
            log.info(f"[V4L2] buf[{i}]: dma_fd={dma_fd} size={size}字节")

            self._bufs.append({'mmap': mm, 'size': size, 'dma_fd': dma_fd,
                                'offset': offset})

    def _queue_all_buffers(self):
        for i in range(self.n_bufs):
            self._qbuf(i)
        log.info(f"[V4L2] 所有 {self.n_bufs} 个 buffer 已入队")

    # ── stream on/off ──

    def _streamon(self):
        buf_type = ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
        _ioctl(self._fd, _VIDIOC_STREAMON, buf_type, 'VIDIOC_STREAMON')
        self._streaming = True
        log.info("[V4L2] STREAMON — 摄像头开始采集")

    def _streamoff(self):
        buf_type = ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
        try:
            _ioctl(self._fd, _VIDIOC_STREAMOFF, buf_type, 'VIDIOC_STREAMOFF')
        except V4L2Error as e:
            log.warning(f"[V4L2] STREAMOFF 警告（忽略）: {e}")
        self._streaming = False
        log.info("[V4L2] STREAMOFF")

    # ── 帧操作 ──

    def _qbuf(self, index: int):
        planes = (v4l2_plane * VIDEO_MAX_PLANES)()
        buf = v4l2_buffer()
        buf.index    = index
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        buf.memory   = V4L2_MEMORY_MMAP
        buf.length   = self._num_planes
        buf.m.planes = planes
        _ioctl(self._fd, _VIDIOC_QBUF, buf, f'VIDIOC_QBUF[{index}]')

    def dequeue(self) -> tuple:
        """
        取出一帧，返回 (dma_fd, buf_index)。
        调用方用完后必须调用 requeue(buf_index) 归还 buffer。
        """
        planes = (v4l2_plane * VIDEO_MAX_PLANES)()
        buf = v4l2_buffer()
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        buf.memory   = V4L2_MEMORY_MMAP
        buf.length   = self._num_planes
        buf.m.planes = planes
        try:
            _ioctl(self._fd, _VIDIOC_DQBUF, buf, 'VIDIOC_DQBUF')
        except V4L2Error as e:
            if e.err == errno_mod.EAGAIN:
                raise BlockingIOError("无可用帧，请用 select() 等待")
            raise
        idx    = buf.index
        dma_fd = self._bufs[idx]['dma_fd']
        log.debug(f"[V4L2] DQBUF index={idx} dma_fd={dma_fd} seq={buf.sequence}")
        return dma_fd, idx

    def requeue(self, buf_index: int):
        self._qbuf(buf_index)
        log.debug(f"[V4L2] QBUF index={buf_index}")

    def wait_frame(self, timeout_sec: float = 2.0) -> tuple:
        """阻塞等待一帧，超时抛出 TimeoutError。返回 (dma_fd, buf_index)"""
        import select
        r, _, _ = select.select([self._fd], [], [], timeout_sec)
        if not r:
            raise TimeoutError(f"[V4L2] 等待帧超时 ({timeout_sec}s)")
        return self.dequeue()

    def iter_frames(self, timeout_sec: float = 2.0):
        while True:
            yield self.wait_frame(timeout_sec)

    # ── 调试工具 ──

    def diag(self):
        print("=" * 50)
        print(f"[V4L2 DIAG] 设备:    {self.device}")
        print(f"[V4L2 DIAG] fd:       {self._fd}")
        print(f"[V4L2 DIAG] 分辨率:  {self.actual_width}x{self.actual_height}")
        print(f"[V4L2 DIAG] 帧大小:  {self._frame_size} 字节")
        print(f"[V4L2 DIAG] 平面数:  {self._num_planes}")
        print(f"[V4L2 DIAG] buffer数: {self.n_bufs}")
        print(f"[V4L2 DIAG] 采集中:  {self._streaming}")
        for i, b in enumerate(self._bufs):
            print(f"[V4L2 DIAG]   buf[{i}]: dma_fd={b['dma_fd']} "
                  f"size={b['size']} offset=0x{b['offset']:x}")
        print("=" * 50)

    def dump_frame_to_file(self, buf_index: int, path: str):
        """
        把某帧原始数据（NV12）写入文件。
        PC端转换: ffmpeg -f rawvideo -pix_fmt nv12 -s 1280x720 -i frame.nv12 frame.png
        """
        b = self._bufs[buf_index]
        if b.get('mmap') is None:
            log.error(f"[V4L2] buf[{buf_index}] 没有 mmap，无法 dump")
            return
        mm = b['mmap']
        mm.seek(0)
        data = mm.read(self._frame_size)
        with open(path, 'wb') as f:
            f.write(data)
        log.info(f"[V4L2] 帧已写入: {path} ({len(data)} 字节)")
