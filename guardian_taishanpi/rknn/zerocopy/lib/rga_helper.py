"""
rga_helper.py — RGA2/RGA3 im2d Python 封装
============================================
通过 ctypes 调用 librga.so，实现：
    dma_buf fd (NV12, WxH) → dma_buf fd (RGB888, 640x640)
    全程无 CPU 参与图像数据搬运。

RGA im2d API 参考：
    https://github.com/airockchip/librga/blob/main/docs/Rockchip_Developer_Guide_RGA_EN.md

librga.so 位置（RK3576 泰山派系统）：
    /usr/lib/librga.so   或   /usr/local/lib/librga.so

调试说明：
    RGAError 携带 im2d 错误码和字符串。
    RGAConverter.diag() 打印 RGA 版本和能力。
    每次 convert() 调用会记录耗时，异常时打印输入/输出 buffer 参数。
"""

import ctypes
import ctypes.util
import logging
import os
import time

log = logging.getLogger(__name__)

# ──────────────────────────────────────────────────────────────
# 加载 librga.so
# ──────────────────────────────────────────────────────────────

def _load_librga() -> ctypes.CDLL:
    candidates = [
        'librga.so',
        '/usr/lib/librga.so',
        '/usr/local/lib/librga.so',
        '/usr/lib/aarch64-linux-gnu/librga.so',
    ]
    for path in candidates:
        try:
            lib = ctypes.CDLL(path)
            log.info(f"[RGA] 加载成功: {path}")
            return lib
        except OSError:
            continue
    raise OSError(
        "[RGA] 找不到 librga.so！\n"
        "  请确认已安装 librga：\n"
        "    sudo apt install librga2  或\n"
        "    sudo apt install librga-dev\n"
        "  也可手动指定路径：export LD_LIBRARY_PATH=/path/to/librga/dir"
    )

# ──────────────────────────────────────────────────────────────
# im2d_type.h 关键常量
# ──────────────────────────────────────────────────────────────

# 像素格式 (RGA_FORMAT_*)
# 来自 rga.h RgaSURF_FORMAT 枚举，im2d API 使用的是不带 <<8 的 index 值
# RK_FORMAT_XXX = index << 8，im2d 传参用 index（低8位）
RGA_FORMAT_RGBA_8888      = 0x0    # RK_FORMAT_RGBA_8888
RGA_FORMAT_RGB_888        = 0x2    # RK_FORMAT_RGB_888
RGA_FORMAT_BGRA_8888      = 0x3    # RK_FORMAT_BGRA_8888（原来误写为BGR_888）
RGA_FORMAT_BGR_888        = 0x7    # RK_FORMAT_BGR_888（正确值，原来误写为0x3）
RGA_FORMAT_YCbCr_420_SP   = 0xa    # RK_FORMAT_YCbCr_420_SP = NV12（原来误写为0x10）

# 图像操作标志（来自 im2d_type.h IM_USAGE 枚举，注意不是 1<<0/1<<1）
IM_SYNC      = 1 << 19  # 同步执行（等待完成后再返回）
IM_ASYNC     = 1 << 26  # 异步执行

# buffer 类型
IM_INPUT_BUFFER  = 1
IM_OUTPUT_BUFFER = 2

# ──────────────────────────────────────────────────────────────
# im2d 结构体
# ──────────────────────────────────────────────────────────────

class im_rect(ctypes.Structure):
    """矩形区域 (x, y, width, height)"""
    _fields_ = [
        ('x',      ctypes.c_int),
        ('y',      ctypes.c_int),
        ('width',  ctypes.c_int),
        ('height', ctypes.c_int),
    ]

class _im_nn_t(ctypes.Structure):
    _fields_ = [
        ('scale_r',  ctypes.c_int),
        ('scale_g',  ctypes.c_int),
        ('scale_b',  ctypes.c_int),
        ('offset_r', ctypes.c_int),
        ('offset_g', ctypes.c_int),
        ('offset_b', ctypes.c_int),
    ]

class _im_colorkey_range(ctypes.Structure):
    _fields_ = [('max', ctypes.c_int), ('min', ctypes.c_int)]

class rga_buffer_t(ctypes.Structure):
    """
    对应 im2d_type.h 中 rga_buffer_t，字段顺序必须与 C 结构体完全一致。

    实际 C 定义（精简后的关键字段）：
        void*  vir_addr;      // offset  0 (64bit: 8字节)
        void*  phy_addr;      // offset  8
        int    fd;            // offset 16
        int    width;         // offset 20
        int    height;        // offset 24
        int    wstride;       // offset 28
        int    hstride;       // offset 32
        int    format;        // offset 36
        int    color_space_mode; // offset 40
        int    global_alpha;  // offset 44  (union with alpha_bit)
        int    rd_mode;       // offset 48
        int    color;         // offset 52  (legacy)
        im_colorkey_range colorkey_range; // offset 56 (8字节)
        im_nn_t nn;           // offset 64 (24字节)
        int    rop_code;      // offset 88
        rga_buffer_handle_t handle; // offset 92 (uint32_t)
    """
    _fields_ = [
        ('vir_addr',         ctypes.c_void_p),   # 必须在 fd 之前
        ('phy_addr',         ctypes.c_void_p),
        ('fd',               ctypes.c_int),
        ('width',            ctypes.c_int),
        ('height',           ctypes.c_int),
        ('wstride',          ctypes.c_int),
        ('hstride',          ctypes.c_int),
        ('format',           ctypes.c_int),
        ('color_space_mode', ctypes.c_int),
        ('global_alpha',     ctypes.c_int),
        ('rd_mode',          ctypes.c_int),
        ('color',            ctypes.c_int),
        ('colorkey_range',   _im_colorkey_range),
        ('nn',               _im_nn_t),
        ('rop_code',         ctypes.c_int),
        ('handle',           ctypes.c_uint32),
    ]


class im_opt(ctypes.Structure):
    """
    对应 im2d_type.h im_opt_t。
    注意：
    1. 第一个字段是 version（im_api_version_t = uint32_t）
    2. im_opt_t 内部有大量嵌套结构，直接用字节数组 + version + 常用字段表示，
       避免完整重现所有嵌套结构体带来的对齐风险。
    3. reserve 实际是 92 字节（非 128）。
    策略：只精确声明 version / priority / core，其余用 padding 填充，
    传给 librga 时大小匹配即可。
    """
    # 总大小约 ~400+ 字节，用粗粒度布局保证不小于实际大小
    _fields_ = [
        ('version',  ctypes.c_uint32),   # im_api_version_t，必须放首位
        ('_padding', ctypes.c_uint8 * 512),  # 足够大的 padding，确保不会越界
    ]

# ──────────────────────────────────────────────────────────────
# 异常类
# ──────────────────────────────────────────────────────────────

class RGAError(Exception):
    """RGA 操作失败时抛出"""
    def __init__(self, msg: str, ret_code: int = 0):
        self.ret_code = ret_code
        hints = {
            -1: "IM_STATUS_FAILED: 操作失败（通用错误）",
            -2: "IM_STATUS_NOERROR: 无错误但未执行",
            -3: "IM_STATUS_NOT_SUPPORTED: 该格式/操作不被 RGA 硬件支持",
            -4: "IM_STATUS_OUT_OF_MEMORY: RGA 内存不足",
            -5: "IM_STATUS_INVALID_PARAM: 参数无效（宽高/格式/对齐要求？）",
        }
        detail = hints.get(ret_code, f"未知错误码: {ret_code}")
        super().__init__(f"[RGA] {msg}\n  错误码: {ret_code} → {detail}")

# ──────────────────────────────────────────────────────────────
# 主类
# ──────────────────────────────────────────────────────────────

class RGAConverter:
    """
    使用 RGA 硬件做图像格式转换和缩放，全程 dma_buf fd 传递，不拷贝像素数据。

    典型用法：
        rga = RGAConverter()
        out_fd, out_handle = rga.alloc_output_buffer(640, 640, RGA_FORMAT_RGB_888)
        rga.convert(
            src_fd=v4l2_dma_fd, src_w=1280, src_h=720,
            src_fmt=RGA_FORMAT_YCbCr_420_SP,
            dst_fd=out_fd,      dst_w=640,  dst_h=640,
            dst_fmt=RGA_FORMAT_RGB_888
        )
        # out_fd 直接传给 rknn.inference
        rga.free_output_buffer(out_handle)
    """

    def __init__(self):
        self._lib = _load_librga()
        self._setup_prototypes()
        self._output_buffers = []  # 记录已分配的 buffer，close() 时统一释放
        self.diag()

    def _setup_prototypes(self):
        """声明 im2d C 函数原型，让 ctypes 做类型检查"""
        lib = self._lib

        # 注意：ctypes.CDLL 的属性访问是懒加载，getattr 对不存在的符号也不会报错，
        # 只有在实际调用时才触发 OSError。因此这里无法通过 try-getattr 可靠地
        # 检测符号是否存在。
        # 实际上 RK3576 泰山派的 librga 必然包含 improcess，此处的 _has_* 标志
        # 主要用于在其他平台上提供回退路径，以及让 diag() 能正确显示状态。
        # 如果需要真正可靠的检测，应在首次调用时捕获 OSError 并更新标志位。
        def _try_setup(name, restype, argtypes):
            try:
                fn = getattr(lib, name)
                fn.restype  = restype
                fn.argtypes = argtypes
                # 做一次 nop 式的可达性验证（不实际调用）
                return True
            except (AttributeError, OSError):
                return False

        # C API (IM_C_API) 版本：7个参数，无 fence/opt
        # IM_API (C++) 版本：10个参数，有 fence/opt
        # ctypes 调用 C 符号，使用 7 参数版本
        self._has_improcess  = _try_setup('improcess', ctypes.c_int, [
            rga_buffer_t, rga_buffer_t, rga_buffer_t,
            im_rect, im_rect, im_rect,
            ctypes.c_int,   # usage
        ])
        self._has_imresize   = _try_setup('imresize', ctypes.c_int, [
            rga_buffer_t, rga_buffer_t,
            ctypes.c_double, ctypes.c_double,
            ctypes.c_int, ctypes.c_int, ctypes.c_int,
        ])
        self._has_imcvtcolor = _try_setup('imcvtcolor', ctypes.c_int, [
            rga_buffer_t, rga_buffer_t,
            ctypes.c_int, ctypes.c_int,
            ctypes.c_int, ctypes.c_int,
        ])
        _try_setup('querystring', ctypes.c_char_p, [ctypes.c_int])
        _try_setup('importbuffer_fd',      ctypes.c_int, [ctypes.c_int, ctypes.c_int])
        _try_setup('releasebuffer_handle', ctypes.c_int, [ctypes.c_int])

        log.info(f"[RGA] 函数可用性: improcess={self._has_improcess} "
                 f"imresize={self._has_imresize} imcvtcolor={self._has_imcvtcolor}")

    def _make_buffer(self, fd: int, w: int, h: int, fmt: int) -> rga_buffer_t:
        """
        构造 rga_buffer_t。
        使用 fd 方式（dma_buf fd）——不需要虚拟地址，RGA 驱动通过 fd 找到物理地址。
        """
        buf = rga_buffer_t()
        buf.fd      = fd
        buf.width   = w
        buf.height  = h
        buf.wstride = w   # 步长=宽度（无 padding）
        buf.hstride = h
        buf.format  = fmt
        return buf

    def alloc_output_buffer(self, width: int, height: int,
                             fmt: int = RGA_FORMAT_RGB_888) -> tuple[int, int]:
        """
        分配 NPU 输入用的 DMA buffer（通过 /dev/dma_heap 或 ion）。
        返回 (dma_fd, handle)。

        注意：此函数使用 /dev/dma_heap/system-uncached，RK3576 Linux 5.10+ 有此接口。
              若不存在会回退到 /dev/ion（旧内核）。
        """
        # 计算所需字节数
        bytes_per_pixel = {
            RGA_FORMAT_RGB_888:      3,
            RGA_FORMAT_BGR_888:      3,
            RGA_FORMAT_RGBA_8888:    4,
            RGA_FORMAT_YCbCr_420_SP: 1,  # NV12: size = w*h*3/2
        }
        bpp = bytes_per_pixel.get(fmt, 3)
        if fmt == RGA_FORMAT_YCbCr_420_SP:
            size = width * height * 3 // 2
        else:
            size = width * height * bpp

        fd = _alloc_dma_buf(size)
        log.info(f"[RGA] 分配输出 buffer: {width}x{height} fmt={fmt} "
                 f"size={size}字节 fd={fd}")
        self._output_buffers.append(fd)
        return fd, fd  # handle 暂用 fd 代替

    def free_output_buffer(self, handle: int):
        """释放由 alloc_output_buffer 分配的 buffer"""
        if handle in self._output_buffers:
            os.close(handle)
            self._output_buffers.remove(handle)
            log.debug(f"[RGA] 释放输出 buffer fd={handle}")

    def convert(self,
                src_fd: int, src_w: int, src_h: int, src_fmt: int,
                dst_fd: int, dst_w: int, dst_h: int, dst_fmt: int,
                dst_x: int = 0, dst_y: int = 0,
                dst_rect_w: int = None, dst_rect_h: int = None,
                sync: bool = True) -> float:
        """
        RGA 硬件缩放+格式转换。
        src: NV12 WxH → dst: RGB dst_w x dst_h

        letterbox 用法（保持原始宽高比，上下/左右加黑边）：
            convert(..., dst_y=pad_top, dst_rect_h=content_h)
        此时 RGA 只写 dst buffer 中 [dst_y : dst_y+dst_rect_h] 行，
        其余行（黑边）需调用方在分配 buffer 后提前清零一次。

        返回耗时（秒）。
        """
        t0 = time.perf_counter()

        # dst 内容区矩形（默认整块）
        cx = dst_x
        cy = dst_y
        cw = dst_rect_w if dst_rect_w is not None else dst_w
        ch = dst_rect_h if dst_rect_h is not None else dst_h

        src_buf = self._make_buffer(src_fd, src_w, src_h, src_fmt)
        dst_buf = self._make_buffer(dst_fd, dst_w, dst_h, dst_fmt)

        log.debug(f"[RGA] convert: src fd={src_fd} {src_w}x{src_h} fmt={src_fmt}"
                  f" → dst fd={dst_fd} {dst_w}x{dst_h} fmt={dst_fmt}"
                  f" dst_rect=({cx},{cy},{cw},{ch})")

        usage = IM_SYNC if sync else IM_ASYNC

        if self._has_improcess:
            src_rect  = im_rect(0, 0, src_w, src_h)
            dst_rect  = im_rect(cx, cy, cw, ch)
            pat_rect  = im_rect()
            empty_buf = rga_buffer_t()
            # C API 7参数版本：无 fence/opt 参数
            ret = self._lib.improcess(
                src_buf, dst_buf, empty_buf,
                src_rect, dst_rect, pat_rect,
                usage,
            )
        else:
            # 回退路径：imresize + imcvtcolor（需要中间 buffer，颜色空间可能不对）
            log.warning("[RGA] improcess 不存在，使用 imresize 回退路径（仅缩放，无格式转换）")
            ret = self._rga_resize_then_cvt(src_buf, dst_buf, src_w, src_h,
                                             dst_w, dst_h, src_fmt, dst_fmt, sync)

        elapsed = time.perf_counter() - t0

        # IM_STATUS_SUCCESS=1 或 IM_STATUS_NOERROR=2 都是成功
        if ret not in (0, 1, 2):
            log.error(f"[RGA] convert 失败 ret={ret}")
            log.error(f"  src: fd={src_fd} {src_w}x{src_h} fmt=0x{src_fmt:x}")
            log.error(f"  dst: fd={dst_fd} {dst_w}x{dst_h} fmt=0x{dst_fmt:x}")
            raise RGAError("improcess 失败", ret)

        log.debug(f"[RGA] convert 完成 耗时={elapsed*1000:.2f}ms ret={ret}")
        return elapsed

    def _rga_resize_then_cvt(self, src, dst, src_w, src_h,
                              dst_w, dst_h, src_fmt, dst_fmt, sync):
        """
        退化路径：仅做 imresize，不做色彩空间转换。
        限制：输出仍是 src_fmt（NV12），不是 dst_fmt（RGB）。
        此路径只在 improcess 不存在时触发，目前泰山派 librga 均包含 improcess，
        正常情况下不会走到这里。若触发此路径，需要额外用 CPU cvtColor 完成转换。
        """
        if not self._has_imresize:
            raise RGAError("improcess 和 imresize 均不可用，无法执行 RGA 转换", -3)
        sync_flag = IM_SYNC if sync else IM_ASYNC
        ret = self._lib.imresize(src, dst, 0.0, 0.0, 0, sync_flag, -1)
        if ret not in (0, 1, 2):
            raise RGAError("imresize 失败", ret)
        log.warning("[RGA] 回退路径：只完成了缩放，未完成 NV12→RGB 格式转换，"
                    "调用方需要用 cv2.cvtColor 补充转换")
        return ret

    def close(self):
        """释放所有已分配的输出 buffer"""
        for fd in list(self._output_buffers):
            self.free_output_buffer(fd)
        log.info("[RGA] 已关闭，所有 buffer 已释放")

    def __enter__(self): return self
    def __exit__(self, *_): self.close()

    def diag(self):
        """打印 RGA 版本和能力信息"""
        print("=" * 50)
        if hasattr(self._lib, 'querystring'):
            try:
                # RGA_VERSION_QUERY=0, RGA_HARDWARE_QUERY=1, RGA_BUF_QUERY=2
                ver = self._lib.querystring(0)
                hw  = self._lib.querystring(1)
                print(f"[RGA DIAG] 版本: {ver.decode(errors='replace') if ver else 'N/A'}")
                print(f"[RGA DIAG] 硬件: {hw.decode(errors='replace') if hw else 'N/A'}")
            except Exception as e:
                print(f"[RGA DIAG] querystring 失败: {e}")
        print(f"[RGA DIAG] improcess: {'有' if self._has_improcess else '无'}")
        print(f"[RGA DIAG] imresize:  {'有' if self._has_imresize else '无'}")
        print(f"[RGA DIAG] imcvtcolor:{'有' if self._has_imcvtcolor else '无'}")
        print("=" * 50)


# ──────────────────────────────────────────────────────────────
# DMA buffer 分配工具（/dev/dma_heap）
# ──────────────────────────────────────────────────────────────

# dma_heap ioctl 定义（来自 linux/dma-heap.h）
class _dma_heap_allocation_data(ctypes.Structure):
    _fields_ = [
        ('len',        ctypes.c_uint64),
        ('fd',         ctypes.c_uint32),
        ('fd_flags',   ctypes.c_uint32),
        ('heap_flags', ctypes.c_uint64),
    ]

_DMA_HEAP_IOC_ALLOC = (0xC0 << 24) | (ctypes.sizeof(_dma_heap_allocation_data) << 16) \
                      | (ord('H') << 8) | 0x0

def _alloc_dma_buf(size: int) -> int:
    """
    通过 /dev/dma_heap/system-uncached 分配连续物理内存，返回 dma_buf fd。
    uncached: CPU 不缓存，适合 DMA 设备（NPU/RGA）直接读写。
    """
    heap_paths = [
        '/dev/dma_heap/system-uncached',
        '/dev/dma_heap/system',
        '/dev/dma_heap/cma',
    ]

    for heap_path in heap_paths:
        if not os.path.exists(heap_path):
            log.debug(f"[DMA] {heap_path} 不存在，跳过")
            continue
        try:
            heap_fd = os.open(heap_path, os.O_RDWR)
            data = _dma_heap_allocation_data()
            data.len      = size
            data.fd_flags = os.O_RDWR | os.O_CLOEXEC
            data.heap_flags = 0
            import fcntl
            fcntl.ioctl(heap_fd, _DMA_HEAP_IOC_ALLOC, data)
            os.close(heap_fd)
            log.info(f"[DMA] 分配成功: {heap_path} size={size} fd={data.fd}")
            return data.fd
        except OSError as e:
            log.warning(f"[DMA] {heap_path} 分配失败: {e}")
            try: os.close(heap_fd)
            except: pass
            continue

    # 回退到 ion（旧内核）
    return _alloc_ion_buf(size)


def _alloc_ion_buf(size: int) -> int:
    """旧内核 /dev/ion 回退路径"""
    ION_HEAP_SYSTEM_CONTIG_MASK = 1 << 1
    ION_IOC_ALLOC = 0xC0284900  # 简化，实际需要根据内核版本确认

    if not os.path.exists('/dev/ion'):
        raise OSError(
            "[DMA] 找不到任何 DMA heap 设备！\n"
            "  检查: ls /dev/dma_heap/  或  ls /dev/ion\n"
            "  RK3576 Linux 5.10+ 应有 /dev/dma_heap/system-uncached"
        )
    raise NotImplementedError(
        "[DMA] /dev/ion 回退路径未完整实现，"
        "请确认 /dev/dma_heap/system-uncached 存在"
    )
