"""
pipeline_carry.py — Guardian 携带模式推理管线
==============================================
携带模式：摄像头随身携带，镜头会晃动。

与放置模式（pipeline_zerocopy.py）的区别：
  - 停用：YOLOv8n-pose 姿态估计 + FallDetector（画面抖动误报率高）
  - 停用：人脸识别（携带时不关注访客身份）
  - 启用：YOLOv8n-det 目标检测（NPU Core 2），辅助视障人士识别障碍物

数据流：
  IMX415 → rkaiq → V4L2 dma_buf [0 copy]
  → RGA 推理路 640×640 letterbox [硬件]
  → RGA 显示路 2304×1296 [硬件]
  → mmap→numpy → rknn.inference（Core 2）[NPU]
  → postprocess_det → 障碍物过滤
  → 画面标注 + MJPEG 推流

运行方式：
    source ~/rknn/venv/bin/activate
    DISPLAY=:0 python3 ~/rknn/zerocopy/pipeline_carry.py

退出：按 q 键 或 Ctrl+C
"""

import os
import sys
import time
import mmap
import re
import logging
import threading
import json
import numpy as np
import cv2
from http.server import HTTPServer, BaseHTTPRequestHandler

# ── 中文渲染支持（PIL）────────────────────────────────────────────
try:
    from PIL import ImageFont, ImageDraw, Image as PILImage
    _FONT_PATH = '/usr/share/fonts/truetype/wqy/wqy-microhei.ttc'
    if not os.path.exists(_FONT_PATH):
        for _p in ['/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc',
                   '/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc',
                   '/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc']:
            if os.path.exists(_p):
                _FONT_PATH = _p
                break
    _font_cache = {}
    def _get_font(size):
        if size not in _font_cache:
            try:
                _font_cache[size] = ImageFont.truetype(_FONT_PATH, size)
            except Exception:
                _font_cache[size] = ImageFont.load_default()
        return _font_cache[size]
    def put_text_cn(img_bgr, text, org, font_size=28, color=(255, 255, 255)):
        img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
        pil_img = PILImage.fromarray(img_rgb)
        draw = ImageDraw.Draw(pil_img)
        font = _get_font(font_size)
        draw.text(org, text, font=font, fill=(color[2], color[1], color[0]))
        return cv2.cvtColor(np.array(pil_img), cv2.COLOR_RGB2BGR)
    _PIL_OK = True
except ImportError:
    _PIL_OK = False
    def put_text_cn(img_bgr, text, org, font_size=28, color=(255, 255, 255)):
        cv2.putText(img_bgr, text, org, cv2.FONT_HERSHEY_SIMPLEX,
                    font_size / 40.0, color, 2)
        return img_bgr

log = logging.getLogger(__name__)
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s.%(msecs)03d %(levelname)s %(message)s',
    datefmt='%H:%M:%S'
)

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from zerocopy.lib.v4l2_helper     import V4L2Capture, V4L2_PIX_FMT_NV12, V4L2Error
from zerocopy.lib.rga_helper      import (RGAConverter, RGAError,
                                           RGA_FORMAT_YCbCr_420_SP,
                                           RGA_FORMAT_RGB_888)
from zerocopy.lib.object_detector import ObjectDetector
from rknnlite.api import RKNNLite

# ── 配置 ──────────────────────────────────────────────────────────
DEVICE       = '/dev/video-camera0'
DET_MODEL    = '/home/lckfb/rknn/yolov8n_det_fp16.rknn'
SRC_W, SRC_H = 3840, 2160
DST_W, DST_H = 640, 640          # NPU 输入尺寸
WINDOW_NAME  = 'Guardian Carry (ZeroCopy)'

# ── 流服务器配置 ───────────────────────────────────────────────
STREAM_PORT    = 8080
STREAM_QUALITY = 60
STREAM_MAX_W   = 1280

# ── Letterbox 参数（同放置模式，保持一致） ─────────────────────────
CONTENT_W = DST_W                       # 640
CONTENT_H = DST_W * SRC_H // SRC_W     # 360
PAD_LEFT  = (DST_W - CONTENT_W) // 2   # 0
PAD_TOP   = (DST_H - CONTENT_H) // 2   # 140

# ── 显示路分辨率 ────────────────────────────────────────────────
DISP_W, DISP_H = 2304, 1296

# ── 检测框颜色（按类别） ────────────────────────────────────────
# 行人=红，车辆=橙，交通标志=黄，室内障碍=蓝
_CLS_COLORS = {
    0:  (0,   50,  255),   # person    红
    1:  (0,   140, 255),   # bicycle   橙
    2:  (0,   140, 255),   # car       橙
    3:  (0,   140, 255),   # motorcycle 橙
    5:  (0,   140, 255),   # bus       橙
    7:  (0,   140, 255),   # truck     橙
    9:  (0,   220, 255),   # traffic light 黄
    11: (0,   220, 255),   # stop sign 黄
    56: (255, 100, 0),     # chair     蓝
    57: (255, 100, 0),     # couch     蓝
    58: (255, 100, 0),     # potted plant 蓝
    59: (255, 100, 0),     # bed       蓝
    60: (255, 100, 0),     # dining table 蓝
    63: (255, 100, 0),     # laptop    蓝
}
_DEFAULT_COLOR = (0, 255, 0)


# ── 流状态 ────────────────────────────────────────────────────────
class _StreamState:
    def __init__(self):
        self.frame_bytes: bytes = b''
        self.lock = threading.Lock()
        self.stats: dict = {
            'mode': 'carry',
            'fps': 0.0,
            'detections': [],   # 当前帧检测结果列表
            'temp': 0,
            'mem_mb': 0,
        }

_stream = _StreamState()


class _MJPEGHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path == '/stream':
            self._handle_stream()
        elif self.path == '/snapshot':
            self._handle_snapshot()
        elif self.path == '/stats':
            self._handle_stats()
        else:
            self.send_response(404)
            self.end_headers()

    def _handle_stream(self):
        self.send_response(200)
        self.send_header('Content-Type',
                         'multipart/x-mixed-replace; boundary=frame')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        try:
            while True:
                with _stream.lock:
                    data = _stream.frame_bytes
                if data:
                    self.wfile.write(
                        b'--frame\r\nContent-Type: image/jpeg\r\n\r\n'
                        + data + b'\r\n'
                    )
                    self.wfile.flush()
                time.sleep(0.04)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _handle_snapshot(self):
        with _stream.lock:
            data = _stream.frame_bytes
        if not data:
            self.send_response(503); self.end_headers(); return
        self.send_response(200)
        self.send_header('Content-Type', 'image/jpeg')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(data)

    def _handle_stats(self):
        with _stream.lock:
            data = json.dumps(_stream.stats).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(data)


def _start_stream_server():
    server = HTTPServer(('0.0.0.0', STREAM_PORT), _MJPEGHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    log.info(f'MJPEG 流服务器启动（携带模式）: http://0.0.0.0:{STREAM_PORT}/stream')


def _push_frame(bgr_frame: np.ndarray, stats: dict):
    h, w = bgr_frame.shape[:2]
    if w > STREAM_MAX_W:
        scale = STREAM_MAX_W / w
        bgr_frame = cv2.resize(bgr_frame, (STREAM_MAX_W, int(h * scale)))
    ok, buf = cv2.imencode('.jpg', bgr_frame,
                           [cv2.IMWRITE_JPEG_QUALITY, STREAM_QUALITY])
    if not ok:
        return
    with _stream.lock:
        _stream.frame_bytes = buf.tobytes()
        _stream.stats.update(stats)


# ── 系统指标 ──────────────────────────────────────────────────────
def _read_meminfo():
    try:
        with open('/proc/meminfo') as f:
            text = f.read()
        def _parse(key):
            m = re.search(rf'{key}:\s+(\d+)', text)
            return int(m.group(1)) // 1024 if m else -1
        return _parse('MemAvailable'), _parse('CmaFree')
    except Exception:
        return -1, -1

def _read_temp_c():
    try:
        with open('/sys/class/thermal/thermal_zone0/temp') as f:
            return int(f.read().strip()) // 1000
    except Exception:
        return -1


# ── 绘制检测结果 ────────────────────────────────────────────────
def draw_detections(frame, detections, sx, sy):
    """
    在显示帧上绘制目标检测框和标签。
    detections: postprocess_det() 的返回值（SRC 坐标系）
    sx, sy: SRC → 显示空间缩放系数
    """
    for det in detections:
        b = det['box']
        x1 = int(b[0] * sx); y1 = int(b[1] * sy)
        x2 = int(b[2] * sx); y2 = int(b[3] * sy)
        color = _CLS_COLORS.get(det['cls'], _DEFAULT_COLOR)

        # 检测框（粗细根据置信度变化）
        thickness = 3 if det['conf'] > 0.65 else 2
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, thickness)

        # 标签：中文类别名 + 置信度
        label = f"{det['name_zh']} {det['conf']:.2f}"
        fs = 24
        # 中文字符宽度约为 fs，ASCII 字符约为 fs//2，估算背景宽度
        label_w = sum(fs if ord(c) > 127 else fs // 2 for c in label) + 8
        # 标签背景
        cv2.rectangle(frame,
                      (x1, y1 - fs - 8),
                      (x1 + label_w, y1),
                      color, -1)
        frame = put_text_cn(frame, label, (x1 + 3, y1 - fs - 4),
                             font_size=fs, color=(255, 255, 255))
    return frame


# ── 主逻辑 ────────────────────────────────────────────────────────
def main():
    # ── 初始化目标检测器（Core 2）──
    log.info('初始化目标检测器（Core 2）...')
    detector = ObjectDetector(model_path=DET_MODEL)
    det_ok = detector.load_model(npu_core=RKNNLite.NPU_CORE_2)
    if not det_ok:
        log.error('目标检测器加载失败，请先转换模型并 scp 到板子')
        log.error(f'  期望路径: {DET_MODEL}')
        log.error('  转换方式见 rknn/convert_det_model.py')
        # 不退出，允许无检测功能运行（方便调试其他流程）
        log.warning('将以无检测模式运行（仅推流）')

    # ── 启动流服务器 ──
    _start_stream_server()

    # ── 初始化 RGA ──
    log.info('初始化 RGA...')
    rga = RGAConverter()
    npu_input_fd, _ = rga.alloc_output_buffer(DST_W, DST_H, RGA_FORMAT_RGB_888)
    npu_input_size  = DST_W * DST_H * 3
    # 黑边清零
    with mmap.mmap(npu_input_fd, npu_input_size, mmap.MAP_SHARED,
                   mmap.PROT_READ | mmap.PROT_WRITE) as mm:
        mm.write(b'\x00' * npu_input_size)
    log.info(f'NPU input buffer fd={npu_input_fd} letterbox pad_top={PAD_TOP}')

    disp_fd, _ = rga.alloc_output_buffer(DISP_W, DISP_H, RGA_FORMAT_RGB_888)
    disp_size  = DISP_W * DISP_H * 3
    log.info(f'Display buffer fd={disp_fd} {DISP_W}x{DISP_H}')

    # ── 初始化摄像头 ──
    log.info('初始化摄像头...')
    cam = V4L2Capture(DEVICE, SRC_W, SRC_H, V4L2_PIX_FMT_NV12, n_bufs=4)
    cam.open()
    log.info('摄像头 OK')

    # ── 显示窗口 ──
    cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
    cv2.setWindowProperty(WINDOW_NAME, cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)

    fps_list    = []
    t_v4l2_list = []
    t_rga_list  = []
    t_npu_list  = []
    t_post_list = []
    frame_count  = 0
    error_count  = 0
    avg_fps      = 0.0
    temp_c       = -1
    mem_mb       = -1

    log.info('携带模式推理主循环启动，按 q 退出')

    try:
        while True:
            t_start = time.perf_counter()

            # ── V4L2 取帧 ──
            t0 = time.perf_counter()
            try:
                src_fd, buf_idx = cam.wait_frame(timeout_sec=2.0)
            except TimeoutError:
                log.warning('等帧超时，重试')
                error_count += 1
                continue
            t_v4l2 = (time.perf_counter() - t0) * 1000

            # ── RGA 转换（两路）──
            t0 = time.perf_counter()
            rga_ok = True
            try:
                rga.convert(
                    src_fd=src_fd,       src_w=SRC_W, src_h=SRC_H,
                    src_fmt=RGA_FORMAT_YCbCr_420_SP,
                    dst_fd=npu_input_fd, dst_w=DST_W, dst_h=DST_H,
                    dst_fmt=RGA_FORMAT_RGB_888,
                    dst_x=PAD_LEFT, dst_y=PAD_TOP,
                    dst_rect_w=CONTENT_W, dst_rect_h=CONTENT_H,
                )
                rga.convert(
                    src_fd=src_fd,  src_w=SRC_W, src_h=SRC_H,
                    src_fmt=RGA_FORMAT_YCbCr_420_SP,
                    dst_fd=disp_fd, dst_w=DISP_W, dst_h=DISP_H,
                    dst_fmt=RGA_FORMAT_RGB_888,
                )
            except RGAError as e:
                log.error(f'RGA 失败: {e}')
                rga_ok = False
            cam.requeue(buf_idx)
            if not rga_ok:
                error_count += 1
                continue
            t_rga = (time.perf_counter() - t0) * 1000

            # ── NPU 推理 ──
            t0 = time.perf_counter()
            detections = []
            if det_ok:
                try:
                    mm = mmap.mmap(npu_input_fd, npu_input_size,
                                   mmap.MAP_SHARED, mmap.PROT_READ)
                    rgb_np = np.frombuffer(
                        mm.read(npu_input_size), dtype=np.uint8
                    ).reshape(DST_H, DST_W, 3).copy()
                    mm.close()
                    detections = detector.process(
                        rgb_np, src_w=SRC_W, src_h=SRC_H,
                        content_w=CONTENT_W, content_h=CONTENT_H,
                        pad_left=PAD_LEFT, pad_top=PAD_TOP,
                        obstacle_only=True,
                    )
                except Exception as e:
                    log.debug(f'NPU 推理异常: {e}')
            t_npu = (time.perf_counter() - t0) * 1000

            # ── 统计 ──
            t_post = 0.0   # 携带模式后处理在 detector.process 内完成
            fps = 1.0 / (time.perf_counter() - t_start + 1e-9)
            fps_list.append(fps);     t_v4l2_list.append(t_v4l2)
            t_rga_list.append(t_rga); t_npu_list.append(t_npu)
            t_post_list.append(t_post)
            for lst in (fps_list, t_v4l2_list, t_rga_list, t_npu_list, t_post_list):
                if len(lst) > 30: lst.pop(0)

            frame_count += 1

            if frame_count % 30 == 0:
                avg_fps  = sum(fps_list) / len(fps_list)
                avg_v4l2 = sum(t_v4l2_list) / len(t_v4l2_list)
                avg_rga  = sum(t_rga_list)  / len(t_rga_list)
                avg_npu  = sum(t_npu_list)  / len(t_npu_list)
                mem_mb, cma_mb = _read_meminfo()
                temp_c = _read_temp_c()
                log.info(
                    f'帧{frame_count:6d}  FPS:{avg_fps:.1f}  '
                    f'v4l2:{avg_v4l2:.0f}ms rga:{avg_rga:.0f}ms '
                    f'npu:{avg_npu:.0f}ms  '
                    f'Detections:{len(detections)}  '
                    f'mem:{mem_mb}MB temp:{temp_c}°C err:{error_count}'
                )

            # ── 构建显示帧 ──
            try:
                mm = mmap.mmap(disp_fd, disp_size, mmap.MAP_SHARED, mmap.PROT_READ)
                disp_np = np.frombuffer(
                    mm.read(disp_size), dtype=np.uint8
                ).reshape(DISP_H, DISP_W, 3).copy()
                mm.close()
                disp = cv2.cvtColor(disp_np, cv2.COLOR_RGB2BGR)
                sx_d = DISP_W / SRC_W
                sy_d = DISP_H / SRC_H
                # 绘制检测结果（SRC坐标 → 显示坐标）
                disp = draw_detections(disp, detections, sx_d, sy_d)
            except Exception:
                disp = np.zeros((DISP_H, DISP_W, 3), dtype=np.uint8)

            # ── HUD ──
            hud_en = f'FPS:{avg_fps:.1f}  Obstacles:{len(detections)}  [Carry Mode]'
            cv2.putText(disp, hud_en, (10, 40),
                        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 200), 2)

            cv2.imshow(WINDOW_NAME, disp)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

            # ── 推流 ──
            try:
                _push_frame(disp, {
                    'mode':   'carry',
                    'fps':    round(avg_fps, 1),
                    'temp':   temp_c if frame_count % 30 == 0 else _stream.stats.get('temp', 0),
                    'mem_mb': mem_mb  if frame_count % 30 == 0 else _stream.stats.get('mem_mb', 0),
                    'detections': [
                        {
                            'cls':     d['cls'],
                            'name':    d['name'],
                            'name_zh': d['name_zh'],
                            'conf':    round(d['conf'], 2),
                            'box':     [round(v, 1) for v in d['box']],
                        }
                        for d in detections
                    ],
                })
            except Exception as e:
                log.debug(f'推流异常: {e}')

    except KeyboardInterrupt:
        log.info('用户中断')

    # ── 清理 ──
    cam.close()
    rga.free_output_buffer(npu_input_fd)
    rga.free_output_buffer(disp_fd)
    rga.close()
    detector.release()
    cv2.destroyAllWindows()
    log.info(f'退出，共处理 {frame_count} 帧')


if __name__ == '__main__':
    main()
