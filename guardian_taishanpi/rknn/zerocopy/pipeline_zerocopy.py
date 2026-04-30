"""
pipeline_zerocopy.py — Guardian 零拷贝推理管线（含摔倒检测）
=============================================================
V4L2 dma_buf → RGA(NV12→RGB, 3840x2160→640x640 letterbox) → RKNN NPU → 姿态分析 → 告警

数据流：
  IMX415 → rkaiq → V4L2 dma_buf [0 copy]
  → RGA 推理路 640×640 letterbox [硬件]
  → RGA 显示路 2304×1296 [硬件]
  → mmap→numpy → rknn.inference [NPU]
  → FallDetector 姿态分析 [CPU]
  → 屏幕叠加骨架 + 告警框

运行方式：
    source ~/rknn/venv/bin/activate
    DISPLAY=:0 python3 ~/rknn/zerocopy/pipeline_zerocopy.py

退出：按 q 键 或 Ctrl+C
"""

import os
import sys
import time
import mmap
import re
import logging
import threading
import queue
import json
import numpy as np
import cv2
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

class _ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    """每个连接独立线程，避免 /stream 长连接阻塞其他请求"""
    daemon_threads = True

# ── 中文渲染支持（PIL）────────────────────────────────────────────
try:
    from PIL import ImageFont, ImageDraw, Image as PILImage
    _FONT_PATH = '/usr/share/fonts/truetype/wqy/wqy-microhei.ttc'
    if not os.path.exists(_FONT_PATH):
        # 备选路径
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
        """在 BGR numpy 图像上绘制中文文字（通过 PIL）"""
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
from zerocopy.lib.v4l2_helper  import V4L2Capture, V4L2_PIX_FMT_NV12, V4L2Error
from zerocopy.lib.rga_helper   import (RGAConverter, RGAError,
                                        RGA_FORMAT_YCbCr_420_SP,
                                        RGA_FORMAT_RGB_888)
from zerocopy.lib.pose_analyzer    import FallDetector, FallState
from zerocopy.lib.postprocess_trunc import postprocess_trunc
from zerocopy.lib.face_recognizer  import FaceRecognizer, UNKNOWN_LABEL, MATCH_THRESH, FACE_DB_PATH
from zerocopy.lib.object_detector  import ObjectDetector
from rknnlite.api import RKNNLite

# ── 配置 ──────────────────────────────────────────────────────
DEVICE     = '/dev/video-camera0'
MODEL_PATH = '/home/lckfb/rknn/yolov8n-pose-trunc-int8.rknn'
DET_MODEL  = '/home/lckfb/rknn/yolov8n_det_fp16.rknn'   # 携带模式目标检测模型
SRC_W, SRC_H = 3840, 2160
DST_W, DST_H = 640, 640       # NPU 输入尺寸
CONF_THRESH  = 0.5
NMS_THRESH   = 0.45
WINDOW_NAME  = 'Guardian Pose (ZeroCopy)'

# ── 流服务器配置 ───────────────────────────────────────────────
STREAM_PORT     = 8080          # MJPEG 流端口
STREAM_QUALITY  = 60            # JPEG 压缩质量 (1-100)
STREAM_MAX_W    = 1280          # 推流分辨率宽（缩小节省带宽）
SNAPSHOT_DIR    = '/tmp/guardian_snapshots'   # 跌倒截图保存目录

# ── 人脸识别配置 ──────────────────────────────────────────────
FACE_RECOGNITION_ENABLED = True   # 设为 False 可完全关闭人脸功能

# ── MQTT 配置（与 guardian_cloud/.env 保持一致）─────────────────
MQTT_BROKER   = 'b1117f90.ala.cn-hangzhou.emqxsl.cn'
MQTT_PORT     = 8883
MQTT_USERNAME = 'user1'
MQTT_PASSWORD = '12345'
MQTT_TLS      = True
MQTT_DEVICE_ID = '001'                              # 与 wifi_config.csv 的 device_id 一致
MQTT_TOPIC_VISION = f'guardian/{MQTT_DEVICE_ID}/vision'
# 视觉状态周期上报间隔（秒），避免每帧都发 MQTT
MQTT_REPORT_INTERVAL = 5.0

# ── 反诈骗监听配置 ────────────────────────────────────────────────
# FunASR HTTP 接口地址（电脑上单独启动的 funasr-service，端口 10097）
ANTISCAM_FUNASR_URL    = 'http://192.168.1.16:10097/asr'
# 每段录音时长（秒），录满后送 ASR 分析
ANTISCAM_SEGMENT_SEC   = 10
# 连续多少秒无陌生人后停止监听（避免短暂离开就停录）
ANTISCAM_STOP_DELAY    = 60
# 风险评分阈值：超过此分值触发告警
ANTISCAM_RISK_THRESH   = 60
# 对话记录保存目录
ANTISCAM_LOG_DIR       = '/tmp/guardian_antiscam'
# 录音采样率
ANTISCAM_SAMPLE_RATE   = 16000

# ── Letterbox 参数（保持 16:9 原始比例）──────────────────────────
# 3840x2160 → 640x360 内容区，上下各 140px 黑边
CONTENT_W = DST_W                        # 640
CONTENT_H = DST_W * SRC_H // SRC_W      # 640*2160//3840 = 360
PAD_LEFT  = (DST_W - CONTENT_W) // 2    # 0
PAD_TOP   = (DST_H - CONTENT_H) // 2    # 140

# ── 显示路 RGA 输出分辨率 ────────────────────────────────────────
DISP_W, DISP_H = 2304, 1296  # 第二路 RGA 输出，用于屏幕显示

SKELETON = [
    (0,1),(0,2),(1,3),(2,4),(5,6),
    (5,7),(7,9),(6,8),(8,10),
    (5,11),(6,12),(11,12),
    (11,13),(13,15),(12,14),(14,16),
]
KP_COLORS = [
    (255,0,0),(255,85,0),(255,170,0),(255,255,0),(170,255,0),
    (85,255,0),(0,255,0),(0,255,85),(0,255,170),(0,255,255),
    (0,170,255),(0,85,255),(0,0,255),(85,0,255),(170,0,255),
    (255,0,255),(255,0,170),
]

# ── MJPEG 流服务器 ───────────────────────────────────────────────

class _StreamState:
    """全局共享的推流状态"""
    def __init__(self):
        self.frame_bytes: bytes = b''          # 最新帧 JPEG 字节
        self.lock = threading.Lock()
        self.snapshot_list: list = []          # 跌倒截图文件名列表
        self.stats: dict = {                   # 实时统计信息
            'fps': 0.0, 'persons': 0,
            'fallen': 0, 'alerts': 0,
            'temp': 0, 'mem_mb': 0,
            'identities': [],                  # 当前帧识别到的身份列表
        }

_stream = _StreamState()
_antiscam_inst = None   # 由 main() 赋值，供 HTTP handler 读取实时状态
os.makedirs(SNAPSHOT_DIR, exist_ok=True)


class _MJPEGHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass   # 关闭访问日志，避免刷屏

    def do_GET(self):
        if self.path == '/stream':
            self._handle_stream()
        elif self.path == '/snapshot':
            self._handle_snapshot()
        elif self.path == '/stats':
            self._handle_stats()
        elif self.path.startswith('/snapshots/'):
            self._handle_snapshot_file()
        elif self.path == '/snapshot_list':
            self._handle_snapshot_list()
        elif self.path == '/face_db':
            self._handle_face_db_list()
        elif self.path == '/dialogs':
            self._handle_dialogs_list()
        elif self.path.startswith('/dialogs/'):
            self._handle_dialog_file()
        elif self.path == '/antiscam_live':
            self._handle_antiscam_live()
        elif self.path == '/health':
            self._json_response({'status': 'ok', 'model_loaded': True})
        else:
            self.send_response(404)
            self.end_headers()

    def do_OPTIONS(self):
        """支持跨域预检"""
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, DELETE, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.end_headers()

    def do_POST(self):
        if self.path == '/enroll':
            self._handle_enroll()
        else:
            self.send_response(404)
            self.end_headers()

    def do_DELETE(self):
        if self.path.startswith('/face_db/'):
            name = self.path[len('/face_db/'):]
            import urllib.parse
            name = urllib.parse.unquote(name)
            self._handle_face_delete(name)
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
                        b'--frame\r\n'
                        b'Content-Type: image/jpeg\r\n\r\n'
                        + data + b'\r\n'
                    )
                    self.wfile.flush()
                time.sleep(0.04)   # ~25fps 上限
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _handle_snapshot(self):
        """返回最新一帧 JPEG 静态图"""
        with _stream.lock:
            data = _stream.frame_bytes
        if not data:
            self.send_response(503)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header('Content-Type', 'image/jpeg')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(data)

    def _handle_stats(self):
        """返回实时统计 JSON"""
        with _stream.lock:
            data = json.dumps(_stream.stats).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(data)

    def _handle_snapshot_list(self):
        """返回跌倒截图列表 JSON"""
        with _stream.lock:
            data = json.dumps(_stream.snapshot_list[-20:]).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(data)

    def _handle_antiscam_live(self):
        """返回反诈骗实时状态：是否录音中 + 本次会话ASR片段列表"""
        inst = _antiscam_inst
        if inst is None:
            self._json_response({'recording': False, 'segments': [], 'accum_score': 0})
            return
        with inst._live_lock:
            segs = list(inst._live_segments)
        # 剩余停止倒计时（秒）
        if inst._recording and inst._last_stranger_time > 0:
            elapsed = time.time() - inst._last_stranger_time
            stop_in = max(0, int(ANTISCAM_STOP_DELAY - elapsed))
        else:
            stop_in = 0
        self._json_response({
            'recording':    inst._recording,
            'accum_score':  inst._accum_score,
            'stop_in':      stop_in,   # 距离自动停录的秒数（0=已停或无陌生人）
            'segments':     segs,
        })

    def _handle_dialogs_list(self):
        """返回反诈骗对话记录文件列表，按时间倒序"""
        try:
            files = []
            if os.path.isdir(ANTISCAM_LOG_DIR):
                for fn in sorted(os.listdir(ANTISCAM_LOG_DIR), reverse=True):
                    if fn.endswith('.json'):
                        fp = os.path.join(ANTISCAM_LOG_DIR, fn)
                        stat = os.stat(fp)
                        # 读取文件做摘要：取最高risk_score和命中类型
                        try:
                            with open(fp, 'r', encoding='utf-8') as f:
                                entries = json.load(f)
                            max_score = max((e.get('risk_score', 0) for e in entries), default=0)
                            hit_set = []
                            for e in entries:
                                hit_set += e.get('hit_types', [])
                            alerted = max_score >= ANTISCAM_RISK_THRESH
                        except Exception:
                            entries = []
                            max_score = 0
                            hit_set = []
                            alerted = False
                        files.append({
                            'file':      fn,
                            'mtime':     int(stat.st_mtime),
                            'segments':  len(entries),
                            'max_score': max_score,
                            'alerted':   alerted,
                            'hits':      list(set(hit_set)),
                        })
            self._json_response(files)
        except Exception as e:
            self._json_response({'error': str(e)}, 500)

    def _handle_dialog_file(self):
        """返回单个对话记录JSON内容"""
        import urllib.parse
        fn = urllib.parse.unquote(self.path[len('/dialogs/'):])
        # 防路径穿越
        fn = os.path.basename(fn)
        fp = os.path.join(ANTISCAM_LOG_DIR, fn)
        if not os.path.isfile(fp):
            self.send_response(404)
            self.end_headers()
            return
        try:
            with open(fp, 'r', encoding='utf-8') as f:
                data = f.read().encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(data)
        except Exception as e:
            self._json_response({'error': str(e)}, 500)

    def _handle_snapshot_file(self):
        """返回指定截图文件"""
        fname = self.path[len('/snapshots/'):]
        fpath = os.path.join(SNAPSHOT_DIR, fname)
        if not os.path.exists(fpath) or '..' in fname:
            self.send_response(404)
            self.end_headers()
            return
        with open(fpath, 'rb') as f:
            data = f.read()
        self.send_response(200)
        self.send_header('Content-Type', 'image/jpeg')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(data)

    # ── 人脸库管理接口 ────────────────────────────────────────────

    def _json_response(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)

    def _handle_face_db_list(self):
        """GET /face_db — 返回人脸库列表"""
        import numpy as np
        from collections import Counter
        if not os.path.exists(FACE_DB_PATH):
            self._json_response({'total': 0, 'persons': []})
            return
        try:
            data  = np.load(FACE_DB_PATH, allow_pickle=True)
            names = list(data['names'])
        except Exception as e:
            self._json_response({'error': str(e)}, 500)
            return
        counts  = Counter(names)
        persons = [{'name': n, 'count': c} for n, c in sorted(counts.items())]
        self._json_response({'total': len(names), 'persons': persons})

    def _handle_enroll(self):
        """POST /enroll — 上传图片录入人脸（multipart/form-data: name + file）"""
        import cgi, numpy as np
        ctype = self.headers.get('Content-Type', '')
        if 'multipart/form-data' not in ctype:
            self._json_response({'error': '需要 multipart/form-data'}, 400)
            return

        # 解析 multipart
        length = int(self.headers.get('Content-Length', 0))
        environ = {
            'REQUEST_METHOD': 'POST',
            'CONTENT_TYPE': ctype,
            'CONTENT_LENGTH': str(length),
        }
        import io
        body = self.rfile.read(length)
        form = cgi.FieldStorage(
            fp=io.BytesIO(body),
            headers=self.headers,
            environ=environ,
        )

        name_field = form.getvalue('name', '').strip()
        if not name_field:
            self._json_response({'error': '姓名不能为空'}, 400)
            return

        file_item = form['file'] if 'file' in form else None
        if file_item is None:
            self._json_response({'error': '缺少 file 字段'}, 400)
            return

        img_bytes = file_item.file.read() if hasattr(file_item, 'file') else b''
        if not img_bytes:
            self._json_response({'error': '图片为空'}, 400)
            return

        # 解码图片
        nparr = np.frombuffer(img_bytes, np.uint8)
        import cv2 as _cv2
        bgr = _cv2.imdecode(nparr, _cv2.IMREAD_COLOR)
        if bgr is None:
            self._json_response({'error': '无法解码图片，请上传 JPG/PNG'}, 422)
            return

        # 使用运行中的 face_recognizer 实例
        global face_recognizer
        if face_recognizer is None or not face_recognizer.is_loaded:
            self._json_response({'error': 'RKNN 人脸模型未加载'}, 503)
            return

        success = face_recognizer.enroll(name_field, bgr)
        if not success:
            self._json_response(
                {'error': '未在图片中检测到清晰人脸，请上传正面照（光线充足、人脸居中）'}, 422)
            return

        face_recognizer.save_db()
        log.info(f'[face_api] 录入成功: {name_field}，共 {face_recognizer.db_count} 条')
        self._json_response({'status': 'ok', 'name': name_field, 'total': face_recognizer.db_count})

    def _handle_face_delete(self, name):
        """DELETE /face_db/{name} — 删除指定姓名"""
        import numpy as np
        if not os.path.exists(FACE_DB_PATH):
            self._json_response({'error': '人脸库为空'}, 404)
            return
        try:
            data     = np.load(FACE_DB_PATH, allow_pickle=True)
            names    = list(data['names'])
            features = data['features']
        except Exception as e:
            self._json_response({'error': str(e)}, 500)
            return

        keep_mask = np.array([n != name for n in names])
        deleted   = int((~keep_mask).sum())
        if deleted == 0:
            self._json_response({'error': f"未找到 '{name}' 的记录"}, 404)
            return

        new_names    = [n for n, k in zip(names, keep_mask) if k]
        new_features = features[keep_mask] if keep_mask.any() else np.zeros((0, features.shape[1]), dtype=np.float32)
        np.savez(FACE_DB_PATH, names=np.array(new_names), features=new_features)

        # 同步更新内存中的识别器
        global face_recognizer
        if face_recognizer is not None:
            face_recognizer.load_db()

        log.info(f'[face_api] 删除 {name} 共 {deleted} 条，剩余 {len(new_names)} 条')
        self._json_response({'status': 'ok', 'deleted': deleted, 'total': len(new_names)})


def _start_stream_server():
    server = _ThreadingHTTPServer(('0.0.0.0', STREAM_PORT), _MJPEGHandler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    log.info(f'MJPEG 流服务器启动: http://0.0.0.0:{STREAM_PORT}/stream')


def _color_correct(bgr: np.ndarray) -> np.ndarray:
    """
    MIPI 面板有专属 ISP gamma 校准，推流帧不经过该校准所以饱和度偏高/偏红。
    用 gamma 压缩模拟 MIPI 面板效果，使网页观感接近 MIPI 显示。
    gamma > 1.0 → 整体变暗/变淡（模拟 MIPI 面板压淡效果）
    gamma 建议范围 1.1~1.5，默认 1.3，可按实际效果调整。
    """
    GAMMA = 1.3
    inv_gamma = 1.0 / GAMMA
    # 预计算查找表（256级，避免逐像素计算）
    table = np.array([
        ((i / 255.0) ** inv_gamma) * 255
        for i in range(256)
    ], dtype=np.uint8)
    return cv2.LUT(bgr, table)


def _push_frame(bgr_frame: np.ndarray, stats: dict):
    """将 BGR 帧压缩为 JPEG 并更新共享状态"""
    # 缩放到推流分辨率
    h, w = bgr_frame.shape[:2]
    if w > STREAM_MAX_W:
        scale = STREAM_MAX_W / w
        bgr_frame = cv2.resize(bgr_frame,
                               (STREAM_MAX_W, int(h * scale)))
    ok, buf = cv2.imencode('.jpg', bgr_frame,
                           [cv2.IMWRITE_JPEG_QUALITY, STREAM_QUALITY])
    if not ok:
        return
    with _stream.lock:
        _stream.frame_bytes = buf.tobytes()
        _stream.stats.update(stats)


def _save_fall_snapshot(bgr_frame: np.ndarray, track_id: int,
                         identity: str = UNKNOWN_LABEL, score: float = -1.0):
    """保存跌倒截图，文件名含时间戳和身份"""
    ts = time.strftime('%Y%m%d_%H%M%S')
    safe_name = identity.replace(' ', '_').replace('/', '_')
    fname = f'fall_{ts}_id{track_id}_{safe_name}.jpg'
    fpath = os.path.join(SNAPSHOT_DIR, fname)
    # 缩放到 1280 宽保存
    h, w = bgr_frame.shape[:2]
    if w > 1280:
        scale = 1280 / w
        bgr_frame = cv2.resize(bgr_frame, (1280, int(h * scale)))
    cv2.imwrite(fpath, bgr_frame,
                [cv2.IMWRITE_JPEG_QUALITY, 85])
    with _stream.lock:
        _stream.snapshot_list.append({
            'file':     fname,
            'time':     time.strftime('%Y-%m-%d %H:%M:%S'),
            'track_id': track_id,
            'identity': identity,
            'score':    round(score, 3) if score >= 0 else None,
        })
    log.info(f'跌倒截图已保存: {fpath}  身份={identity}({score:.2f})'
             if score >= 0 else f'跌倒截图已保存: {fpath}  身份=未知')


# ── 系统指标 ──────────────────────────────────────────────────────

def _read_meminfo():
    """返回 (avail_mb, cma_free_mb)"""
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
    """读取 thermal_zone0 温度（°C），失败返回 -1"""
    try:
        with open('/sys/class/thermal/thermal_zone0/temp') as f:
            return int(f.read().strip()) // 1000
    except Exception:
        return -1


# ── 后处理 ──────────────────────────────────────────────────────

def _sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -88, 88)))

def _nms(boxes, scores, iou_thresh):
    x1,y1,x2,y2 = boxes[:,0],boxes[:,1],boxes[:,2],boxes[:,3]
    areas = (x2-x1)*(y2-y1)
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        i = order[0]; keep.append(i)
        xx1=np.maximum(x1[i],x1[order[1:]]); yy1=np.maximum(y1[i],y1[order[1:]])
        xx2=np.minimum(x2[i],x2[order[1:]]); yy2=np.minimum(y2[i],y2[order[1:]])
        w=np.maximum(0,xx2-xx1); h=np.maximum(0,yy2-yy1)
        iou = w*h/(areas[i]+areas[order[1:]]-w*h+1e-6)
        order = order[1:][iou < iou_thresh]
    return keep

def postprocess(output, orig_w, orig_h):
    pred = output[0][0].T   # (8400, 56)
    # RKNN 模型输出已内置 sigmoid，直接取原始值，不再重复 sigmoid
    conf = pred[:, 4]
    mask = conf > CONF_THRESH
    if mask.sum() == 0:
        return [], [], []
    if mask.sum() > 50:
        top = np.argsort(conf[mask])[-50:]
        tmp = np.where(mask)[0][top]
        mask = np.zeros(len(mask), dtype=bool)
        mask[tmp] = True

    boxes_xywh = pred[mask, :4]
    conf = conf[mask]
    kps  = pred[mask, 5:]

    sx = orig_w / CONTENT_W   # 3840/640 = 6.0
    sy = orig_h / CONTENT_H   # 2160/360 = 6.0
    boxes = boxes_xywh.copy()
    boxes[:,0] = boxes_xywh[:,0] - boxes_xywh[:,2]/2
    boxes[:,1] = boxes_xywh[:,1] - boxes_xywh[:,3]/2
    boxes[:,2] = boxes_xywh[:,0] + boxes_xywh[:,2]/2
    boxes[:,3] = boxes_xywh[:,1] + boxes_xywh[:,3]/2
    # 去掉 letterbox padding 偏移，再映射回原始分辨率
    boxes[:,0] = (boxes[:,0] - PAD_LEFT) * sx
    boxes[:,2] = (boxes[:,2] - PAD_LEFT) * sx
    boxes[:,1] = (boxes[:,1] - PAD_TOP)  * sy
    boxes[:,3] = (boxes[:,3] - PAD_TOP)  * sy

    valid = ((boxes[:,0]>=0)&(boxes[:,1]>=0)&
             (boxes[:,2]<=orig_w)&(boxes[:,3]<=orig_h)&
             (boxes[:,2]>boxes[:,0])&(boxes[:,3]>boxes[:,1]))
    if valid.sum() == 0: return [], [], []
    boxes = boxes[valid]; conf = conf[valid]; kps = kps[valid]

    keep = _nms(boxes, conf, NMS_THRESH)
    boxes = boxes[keep]; conf = conf[keep]; kps = kps[keep]

    kps_out = []
    for kp in kps:
        kp = kp.reshape(17,3)
        kp[:,0] = (kp[:,0] - PAD_LEFT) * sx
        kp[:,1] = (kp[:,1] - PAD_TOP)  * sy
        kps_out.append(kp)
    return boxes, conf, kps_out

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
except ImportError:
    def put_text_cn(img_bgr, text, org, font_size=28, color=(255, 255, 255)):
        cv2.putText(img_bgr, text, org, cv2.FONT_HERSHEY_SIMPLEX,
                    font_size / 40.0, color, 2)
        return img_bgr

# ── 检测框颜色（按类别） ────────────────────────────────────────
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


def draw_detections(frame, detections, sx, sy):
    """在显示帧上绘制目标检测框和标签。"""
    for det in detections:
        b = det['box']
        x1 = int(b[0] * sx); y1 = int(b[1] * sy)
        x2 = int(b[2] * sx); y2 = int(b[3] * sy)
        color = _CLS_COLORS.get(det['cls'], _DEFAULT_COLOR)
        thickness = 3 if det['conf'] > 0.65 else 2
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, thickness)
        label = f"{det['name_zh']} {det['conf']:.2f}"
        fs = 24
        label_w = sum(fs if ord(c) > 127 else fs // 2 for c in label) + 8
        cv2.rectangle(frame, (x1, y1 - fs - 8), (x1 + label_w, y1), color, -1)
        frame = put_text_cn(frame, label, (x1 + 3, y1 - fs - 4),
                            font_size=fs, color=(255, 255, 255))
    return frame


def draw_pose(frame, boxes, confs, kps_list):
    return draw_pose_scaled(frame, boxes, confs, kps_list, 1.0, 1.0)

def draw_pose_scaled(frame, boxes, confs, kps_list, sx, sy):
    for box, conf, kps in zip(boxes, confs, kps_list):
        x1,y1,x2,y2 = (box * [sx,sy,sx,sy]).astype(int)
        cv2.rectangle(frame,(x1,y1),(x2,y2),(0,255,0),2)
        cv2.putText(frame,f'{conf:.2f}',(x1,y1-5),
                    cv2.FONT_HERSHEY_SIMPLEX,0.5,(0,255,0),1)
        for (i,j) in SKELETON:
            if kps[i,2]>0.3 and kps[j,2]>0.3:
                cv2.line(frame,(int(kps[i,0]*sx),int(kps[i,1]*sy)),
                               (int(kps[j,0]*sx),int(kps[j,1]*sy)),(0,255,255),2)
        for k,(x,y,v) in enumerate(kps):
            if v>0.3:
                cv2.circle(frame,(int(x*sx),int(y*sy)),4,KP_COLORS[k],-1)
    return frame


def draw_fall_overlay(frame, person_states, sx, sy, identities=None):
    """
    在显示帧上叠加摔倒检测结果。
    person_states: List[PersonState]，坐标为原始 SRC 分辨率空间。
    sx, sy: SRC → 显示空间缩放系数。
    identities: list of (name, score, face_box) 与 person_states 一一对应，可为 None。
    """
    for i, ps in enumerate(person_states):
        if ps.box is None:
            continue
        x1, y1, x2, y2 = (ps.box * [sx, sy, sx, sy]).astype(int)

        # 取身份信息
        identity, id_score, face_box = (UNKNOWN_LABEL, -1.0, None)
        if identities and i < len(identities):
            identity, id_score, face_box = identities[i]

        # 身份标签字符串
        if identity != UNKNOWN_LABEL and id_score >= 0:
            id_label = f'{identity} {id_score:.2f}'
        elif identity != UNKNOWN_LABEL:
            id_label = identity
        else:
            id_label = ''

        if ps.state == FallState.FALLEN:
            # 红色粗框 + 告警文字
            cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 0, 255), 4)
            label = f'[FALL] {id_label or f"ID:{ps.track_id}"}'
            # 半透明红色背景标签
            fs = 26
            cv2.rectangle(frame, (x1, y1 - fs - 8), (x1 + len(label) * fs // 2 + 6, y1), (0, 0, 200), -1)
            frame = put_text_cn(frame, label, (x1 + 3, y1 - fs - 4), font_size=fs, color=(255, 255, 255))
            # 特征调试信息（纯英文数字，cv2.putText 即可）
            if ps.features:
                feat = ps.features
                dbg = (f'bbox:{feat.bbox_ratio:.2f} '
                       f'trunk:{feat.trunk_angle:.1f}deg '
                       f'head:{feat.head_height_ratio:.2f}')
                cv2.putText(frame, dbg, (x1, y2 + 20),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 100, 255), 1)

        elif ps.state == FallState.SUSPICIOUS:
            # 黄色框
            cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 200, 255), 3)
            label = f'[SUSP] {id_label or f"ID:{ps.track_id}"}'
            frame = put_text_cn(frame, label, (x1, y1 - 30), font_size=26, color=(0, 200, 255))

        else:
            # 正常绿色框
            cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)
            label = id_label if id_label else f'ID:{ps.track_id} {ps.conf:.2f}'
            frame = put_text_cn(frame, label, (x1, y1 - 30), font_size=24, color=(0, 255, 0))

        # 在人脸框上画蓝色小框 + 姓名标签（仅当检测到人脸时）
        if face_box is not None:
            fx1 = int(face_box[0] * sx); fy1 = int(face_box[1] * sy)
            fx2 = int(face_box[2] * sx); fy2 = int(face_box[3] * sy)
            cv2.rectangle(frame, (fx1, fy1), (fx2, fy2), (255, 100, 0), 2)
            if id_label:
                frame = put_text_cn(frame, id_label, (fx1, max(0, fy1 - 30)),
                                    font_size=24, color=(255, 100, 0))

    return frame


# ── MQTT 客户端 ───────────────────────────────────────────────────────
class _MQTTPublisher:
    """
    后台 MQTT 发布器，使用独立线程避免阻塞主循环。
    发布失败不影响推理，自动重连。
    """
    def __init__(self):
        self._client = None
        self._connected = False
        self._queue: queue.Queue = queue.Queue(maxsize=32)
        self._thread = threading.Thread(target=self._worker, daemon=True)
        self._thread.start()

    def _worker(self):
        import paho.mqtt.client as mqtt
        import ssl

        def on_connect(client, userdata, flags, rc, props=None):
            if rc == 0:
                self._connected = True
                log.info('MQTT 已连接')
            else:
                log.warning(f'MQTT 连接失败 rc={rc}')

        def on_disconnect(client, userdata, rc, props=None, reason=None):
            self._connected = False
            log.warning('MQTT 断开连接，将自动重连')

        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                             client_id=f'guardian-taishanpi-{MQTT_DEVICE_ID}')
        client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
        if MQTT_TLS:
            client.tls_set(cert_reqs=ssl.CERT_NONE)
            client.tls_insecure_set(True)
        client.on_connect    = on_connect
        client.on_disconnect = on_disconnect

        try:
            client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        except Exception as e:
            log.error(f'MQTT 初始连接失败: {e}')

        client.loop_start()
        self._client = client

        while True:
            try:
                topic, payload = self._queue.get(timeout=1.0)
                if self._connected:
                    client.publish(topic, payload, qos=0, retain=False)
            except queue.Empty:
                pass
            except Exception as e:
                log.debug(f'MQTT 发布异常: {e}')

    def publish(self, topic: str, payload: dict):
        """非阻塞发布，队列满则丢弃（不阻塞推理主循环）"""
        try:
            self._queue.put_nowait((topic, json.dumps(payload, ensure_ascii=False)))
        except queue.Full:
            pass


_mqtt = _MQTTPublisher()

# 人脸识别器全局引用（由 run_pipeline 初始化后赋值，供 HTTP 接口使用）
face_recognizer = None


# ── 反诈骗监听器 ──────────────────────────────────────────────────────
class _AntiScamMonitor:
    """
    陌生人反诈骗监听器。
    - 检测到陌生人时自动开始录音（sounddevice，非阻塞回调，不影响 NPU 主循环）
    - 每 ANTISCAM_SEGMENT_SEC 秒将录音片段送 FunASR HTTP 接口转文字
    - 用本地关键词加权规则引擎分析是否存在诈骗话术
    - 风险分超阈值时通过 MQTT 发布告警，并保存对话记录
    - 连续 ANTISCAM_STOP_DELAY 秒无陌生人时自动停止录音
    """

    # ── 反诈骗话术关键词库（类型 → 权重 → 关键词列表）────────────────
    # 权重说明：高危=50（单词即告警），中危=20，低危=10（需多个累积）
    SCAM_KEYWORDS = {
        # 冒充公检法（高危）
        '冒充公检法': (50, [
            '安全账户', '安权账户', '安泉账户', '安全帐户',  # ASR误识别变体
            '涉嫌洗钱', '涉嫌诈骗', '配合调查', '传票',
            '检察院', '公安局', '警察', '案件', '冻结账户', '冻结帐户',
            '资金清查', '保密', '不能告诉家人', '不要告诉别人',
            '涉案资金', '监管账户', '监管帐户',
        ]),
        # 网络刷单（高危）
        '网络刷单': (50, [
            '刷单', '刷信誉', '返佣金', '先垫付', '任务单',
            '兼职赚钱', '轻松日入', '佣金', '抢单',
            '做任务', '接单', '刷好评',
        ]),
        # 贷款诈骗（高危）
        '贷款诈骗': (50, [
            '免息贷款', '免抵押', '额度激活', '手续费', '保证金',
            '流水不够', '刷流水', '激活额度', '下款', '放贷',
            '贷款秒批', '无需征信',
        ]),
        # 冒充熟人/领导（中危）
        '冒充熟人': (20, [
            '换号了', '借钱', '急用', '帮个忙', '转账',
            '我是你朋友', '不要告诉', '先打过来',
            '领导让我', '老板叫我',
        ]),
        # 投资理财诈骗（中危）
        '投资诈骗': (20, [
            '内部消息', '稳赚', '带你操作', '投资平台', '高收益',
            '理财', '收益率', '跑路', '提现', '充值',
            '跟单', '分析师', '股票内幕',
        ]),
        # 中奖诈骗（中危）
        '中奖诈骗': (20, [
            '恭喜中奖', '领奖', '手续费', '中了', '抽奖',
            '大奖', '兑换', '验证码', '核实身份',
            '幸运用户', '免费领取',
        ]),
        # 通用危险词（低危，需累积）
        '通用危险': (10, [
            '转账', '汇款', '银行卡', '密码', '验证码',
            '网银', '支付宝', '微信转账', '紧急',
            '马上', '立刻', '现在转', '赶快',
        ]),
    }

    def __init__(self, mqtt_publisher: _MQTTPublisher):
        self._mqtt = mqtt_publisher
        self._recording  = False          # 当前是否在录音
        self._audio_buf  = []             # 录音缓冲（int16 numpy 片段列表）
        self._buf_lock   = threading.Lock()
        self._last_stranger_time = 0.0   # 上次检测到陌生人的时间
        self._worker_thread: threading.Thread | None = None
        self._stop_evt   = threading.Event()
        self._dialog_log = []             # 本次监听的对话记录
        # 跨段累积分数（60秒滑动窗口，防止诈骗话术被分割到不同片段）
        self._accum_score  = 0            # 累积风险分
        self._accum_hits   = []           # 累积命中类型
        self._accum_texts  = []           # 累积识别文本
        self._accum_reset_time = 0.0      # 上次重置时间
        self._ACCUM_WINDOW = 60           # 累积窗口秒数
        # 实时状态（供HTTP /antiscam_live接口读取）
        self._live_segments = []          # 本次录音会话的ASR片段列表（最多保留50条）
        self._live_lock     = threading.Lock()
        os.makedirs(ANTISCAM_LOG_DIR, exist_ok=True)

    # ── 外部调用：通知当前帧是否有陌生人 ──────────────────────────
    def notify_stranger(self, has_stranger: bool):
        """主循环每帧调用，传入当前是否检测到陌生人。"""
        if has_stranger:
            self._last_stranger_time = time.time()
            if not self._recording:
                self._start_recording()
        else:
            # 超过 STOP_DELAY 秒无陌生人才停止
            if self._recording:
                elapsed = time.time() - self._last_stranger_time
                if elapsed >= ANTISCAM_STOP_DELAY:
                    self._stop_recording()

    # ── 启动录音 ──────────────────────────────────────────────────
    def _start_recording(self):
        if self._recording:
            return
        try:
            import sounddevice as sd
        except ImportError:
            log.warning('[反诈骗] sounddevice 未安装，跳过录音 (pip install sounddevice)')
            return

        self._recording = True
        self._audio_buf.clear()
        self._dialog_log.clear()
        self._stop_evt.clear()
        # 新会话开始，清空实时片段
        with self._live_lock:
            self._live_segments.clear()
        log.info('[反诈骗] 检测到陌生人，开始录音监听')

        def _audio_callback(indata, frames, time_info, status):
            if status:
                log.debug(f'[反诈骗] 录音 status: {status}')
            with self._buf_lock:
                self._audio_buf.append(indata.copy())

        self._stream_obj = sd.InputStream(
            samplerate=ANTISCAM_SAMPLE_RATE,
            channels=1,
            dtype='int16',
            blocksize=ANTISCAM_SAMPLE_RATE // 10,   # 100ms 一块
            callback=_audio_callback,
            device=0,   # es8388 (hw:0,0)，Main Mic Switch=on
        )
        self._stream_obj.start()

        # 启动分析工作线程
        self._worker_thread = threading.Thread(
            target=self._analysis_worker, daemon=True)
        self._worker_thread.start()

    # ── 停止录音 ──────────────────────────────────────────────────
    def _stop_recording(self):
        if not self._recording:
            return
        self._recording = False
        self._stop_evt.set()
        try:
            self._stream_obj.stop()
            self._stream_obj.close()
        except Exception:
            pass
        log.info('[反诈骗] 陌生人离开，停止录音监听')
        # 保存本次对话记录
        if self._dialog_log:
            self._save_dialog_log()

    # ── 分析工作线程：每 SEGMENT_SEC 秒取一段音频送 ASR → 规则分析 ──
    def _analysis_worker(self):
        import numpy as _np
        segment_samples = ANTISCAM_SAMPLE_RATE * ANTISCAM_SEGMENT_SEC
        accumulated = 0   # 已累积的采样数

        while not self._stop_evt.is_set():
            time.sleep(0.5)
            with self._buf_lock:
                if not self._audio_buf:
                    continue
                chunks = self._audio_buf.copy()
                self._audio_buf.clear()

            if not chunks:
                continue

            audio = _np.concatenate(chunks, axis=0).flatten()
            accumulated += len(audio)

            # 未达到一个片段长度则继续积累
            if accumulated < segment_samples:
                # 把剩余数据放回缓冲（简单追加）
                with self._buf_lock:
                    self._audio_buf.insert(0, audio.reshape(-1, 1))
                accumulated = len(audio)   # 重新计算（只有未满段才回放）
                continue

            # 满足一段，取前 segment_samples 采样
            segment = audio[:segment_samples]
            accumulated = 0

            # 调用 ASR
            text = self._asr(segment)
            if not text:
                continue

            log.info(f'[反诈骗] ASR: {text}')

            # 规则引擎分析（本段）
            risk_score, hit_types = self._analyze(text)
            entry = {
                'time':       time.strftime('%Y-%m-%d %H:%M:%S'),
                'text':       text,
                'risk_score': risk_score,
                'hit_types':  hit_types,
            }
            self._dialog_log.append(entry)

            # 实时推送到 /antiscam_live（最多保留50段）
            with self._live_lock:
                self._live_segments.append(entry)
                if len(self._live_segments) > 50:
                    self._live_segments.pop(0)

            # 跨段累积：60秒窗口内分数叠加
            now_ts = time.time()
            if now_ts - self._accum_reset_time > self._ACCUM_WINDOW:
                # 超过窗口，重置累积器
                self._accum_score = 0
                self._accum_hits  = []
                self._accum_texts = []
                self._accum_reset_time = now_ts

            self._accum_score += risk_score
            self._accum_hits  += hit_types
            self._accum_texts.append(text)

            log.info(f'[反诈骗] 本段分={risk_score} 累积分={self._accum_score} 命中={hit_types}')

            if self._accum_score >= ANTISCAM_RISK_THRESH:
                # 触发告警，合并累积内容
                alert_entry = {
                    'time':       time.strftime('%Y-%m-%d %H:%M:%S'),
                    'text':       ' | '.join(self._accum_texts),
                    'risk_score': self._accum_score,
                    'hit_types':  list(set(self._accum_hits)),
                }
                self._trigger_alert(alert_entry)
                # 告警后重置，防止重复告警
                self._accum_score = 0
                self._accum_hits  = []
                self._accum_texts = []
                self._accum_reset_time = now_ts

    # ── ASR：将音频片段 POST 到 FunASR HTTP 服务 ────────────────────
    def _asr(self, audio_int16) -> str:
        """发送 16kHz mono int16 PCM 到 FunASR HTTP，返回识别文字。"""
        import io, wave, urllib.request, urllib.error
        import numpy as _np
        # 写成 WAV bytes
        buf = io.BytesIO()
        with wave.open(buf, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)   # int16 = 2 bytes
            wf.setframerate(ANTISCAM_SAMPLE_RATE)
            wf.writeframes(audio_int16.astype(_np.int16).tobytes())
        wav_bytes = buf.getvalue()

        try:
            req = urllib.request.Request(
                ANTISCAM_FUNASR_URL,
                data=wav_bytes,
                headers={'Content-Type': 'audio/wav'},
                method='POST',
            )
            with urllib.request.urlopen(req, timeout=15) as resp:
                result = json.loads(resp.read().decode())
            # FunASR HTTP 服务返回格式：{"text": "..."}
            return result.get('text', '').strip()
        except (urllib.error.URLError, Exception) as e:
            log.warning(f'[反诈骗] ASR 请求失败: {e}')
            return ''

    # ── 规则引擎：关键词加权匹配 ────────────────────────────────────
    def _analyze(self, text: str):
        """
        返回 (risk_score, hit_types)。
        risk_score: 累计风险分（>=ANTISCAM_RISK_THRESH 触发告警）
        hit_types:  命中的诈骗类型列表
        """
        total_score = 0
        hit_types = []
        for scam_type, (weight, keywords) in self.SCAM_KEYWORDS.items():
            hits = [kw for kw in keywords if kw in text]
            if hits:
                total_score += weight * len(hits)
                hit_types.append(f'{scam_type}({",".join(hits)})')
        return total_score, hit_types

    # ── 触发告警 ──────────────────────────────────────────────────
    def _trigger_alert(self, entry: dict):
        log.warning(
            f'[反诈骗 告警] 风险分={entry["risk_score"]} '
            f'命中={entry["hit_types"]} 文字="{entry["text"]}"'
        )
        self._mqtt.publish(
            f'guardian/{MQTT_DEVICE_ID}/antiscam',
            {
                'event':      'scam_alert',
                'risk_score': entry['risk_score'],
                'hit_types':  entry['hit_types'],
                'text':       entry['text'],
                'ts':         int(time.time()),
            }
        )

    # ── 保存本次对话记录 ──────────────────────────────────────────
    def _save_dialog_log(self):
        ts = time.strftime('%Y%m%d_%H%M%S')
        fpath = os.path.join(ANTISCAM_LOG_DIR, f'dialog_{ts}.json')
        try:
            with open(fpath, 'w', encoding='utf-8') as f:
                json.dump(self._dialog_log, f, ensure_ascii=False, indent=2)
            log.info(f'[反诈骗] 对话记录已保存: {fpath}')
        except Exception as e:
            log.warning(f'[反诈骗] 保存对话记录失败: {e}')





# ── 告警输出 ──────────────────────────────────────────────────────────

def _on_fall_alert(ps, frame_no: int, identity: str = UNKNOWN_LABEL, score: float = -1.0):
    """
    摔倒告警回调。在此处扩展告警输出方式：
      - 当前：logging.WARNING（会写入 journald/syslog）
      - 预留：UART 推送、HTTP webhook、GPIO 触发
    """
    feat = ps.features
    feat_str = ''
    if feat:
        feat_str = (f' bbox={feat.bbox_ratio:.2f}'
                    f' trunk={feat.trunk_angle:.1f}deg'
                    f' head={feat.head_height_ratio:.2f}')

    id_str = f'{identity}({score:.2f})' if score >= 0 else identity
    log.warning(
        f"[FALL ALERT] frame={frame_no} ID={ps.track_id} 身份={id_str}{feat_str}"
    )

    # ── MQTT 发布跌倒事件 ──
    _mqtt.publish(MQTT_TOPIC_VISION, {
        'event':    'fall',
        'track_id': ps.track_id,
        'identity': identity,
        'score':    round(score, 3) if score >= 0 else -1.0,
        'frame':    frame_no,
        'ts':       int(time.time()),
    })


# ── 主逻辑 ──────────────────────────────────────────────────────

def main(mode: str = 'place'):
    log.info(f"启动模式: {'放置模式（跌倒检测+人脸识别）' if mode == 'place' else '携带模式（目标检测）'}")

    # ── 放置模式：初始化 pose 模型（Core 0）──
    rknn = None
    if mode == 'place':
        log.info("初始化 RKNN（pose 模型，Core 0）...")
        rknn = RKNNLite()
        assert rknn.load_rknn(MODEL_PATH) == 0, f"加载模型失败: {MODEL_PATH}"
        assert rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0, "init_runtime 失败"
        log.info("RKNN pose 模型 OK（Core 0）")

    # ── 启动 MJPEG 流服务器 ──
    _start_stream_server()

    # ── 初始化人脸识别器（Core 1，仅放置模式）──
    global face_recognizer
    face_rec = None
    if mode == 'place' and FACE_RECOGNITION_ENABLED:
        face_rec = FaceRecognizer()
        ok = face_rec.load_models(npu_core=RKNNLite.NPU_CORE_1)
        if ok:
            face_rec.load_db()
            log.info(f'人脸识别器 OK（Core 1），人脸库 {face_rec.db_count} 条')
        else:
            log.warning('人脸识别器加载失败，将跳过人脸功能（pose 正常运行）')
            face_rec = None
    face_recognizer = face_rec   # 暴露为全局，供 HTTP 人脸库接口使用

    # ── 初始化目标检测器（Core 0，仅携带模式）──
    detector = None
    if mode == 'carry':
        detector = ObjectDetector(model_path=DET_MODEL)
        det_ok = detector.load_model(npu_core=RKNNLite.NPU_CORE_0)
        if not det_ok:
            log.error(f'目标检测器加载失败，期望路径: {DET_MODEL}')
            detector = None

    log.info("初始化 RGA...")
    rga = RGAConverter()
    # 推理路 buffer（640×640 letterbox）
    npu_input_fd, _ = rga.alloc_output_buffer(DST_W, DST_H, RGA_FORMAT_RGB_888)
    npu_input_size  = DST_W * DST_H * 3
    # 黑边清零（只需一次，RGA 每帧只写内容区，黑边保持不变）
    with mmap.mmap(npu_input_fd, npu_input_size, mmap.MAP_SHARED,
                   mmap.PROT_READ | mmap.PROT_WRITE) as mm:
        mm.write(b'\x00' * npu_input_size)
    log.info(f"NPU input buffer fd={npu_input_fd} letterbox pad_top={PAD_TOP}")
    # 显示路 buffer
    disp_fd, _ = rga.alloc_output_buffer(DISP_W, DISP_H, RGA_FORMAT_RGB_888)
    disp_size  = DISP_W * DISP_H * 3
    log.info(f"Display buffer fd={disp_fd} {DISP_W}x{DISP_H}")

    log.info("初始化摄像头...")
    cam = V4L2Capture(DEVICE, SRC_W, SRC_H, V4L2_PIX_FMT_NV12, n_bufs=4)
    cam.open()
    log.info("摄像头 OK")

    # ── 摔倒检测器（仅放置模式）──
    fall_detector = FallDetector() if mode == 'place' else None
    if fall_detector:
        log.info("摔倒检测器 OK")

    # ── 反诈骗监听器（仅放置模式）──
    antiscam = _AntiScamMonitor(_mqtt) if mode == 'place' else None
    if antiscam:
        log.info("反诈骗监听器 OK")
        global _antiscam_inst
        _antiscam_inst = antiscam

    # ── 窗口标题随模式变化 ──
    window_name = 'Guardian [放置模式]' if mode == 'place' else 'Guardian [携带模式]'
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.setWindowProperty(window_name, cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)
    SHOW_DISPLAY = True

    fps_list    = []
    t_v4l2_list = []
    t_rga_list  = []
    t_npu_list  = []
    t_post_list = []
    frame_count  = 0
    error_count  = 0
    fall_count   = 0   # 累计告警次数
    avg_fps      = 0.0
    last_mqtt_report = 0.0   # 上次周期上报的时间戳

    # 人脸识别锁定缓存：{track_id: (name, score)}
    # 一旦识别成功就锁定，直到该 track_id 消失
    identity_cache = {}

    log.info("开始推理主循环，按 q 退出")

    try:
        while True:
            t_start = time.perf_counter()

            # ── V4L2 取帧 ──
            t0 = time.perf_counter()
            try:
                src_fd, buf_idx = cam.wait_frame(timeout_sec=2.0)
            except TimeoutError:
                log.warning("等帧超时，重试")
                error_count += 1
                continue
            t_v4l2 = (time.perf_counter() - t0) * 1000

            # ── RGA 转换（两路，src buffer 用完后再归还）──
            t0 = time.perf_counter()
            rga_ok = True
            try:
                # 推理路：NV12 → RGB letterbox 640×640
                rga.convert(
                    src_fd=src_fd,       src_w=SRC_W, src_h=SRC_H,
                    src_fmt=RGA_FORMAT_YCbCr_420_SP,
                    dst_fd=npu_input_fd, dst_w=DST_W, dst_h=DST_H,
                    dst_fmt=RGA_FORMAT_RGB_888,
                    dst_x=PAD_LEFT, dst_y=PAD_TOP,
                    dst_rect_w=CONTENT_W, dst_rect_h=CONTENT_H,
                )
                # 显示路：NV12 → RGB DISP_W×DISP_H
                rga.convert(
                    src_fd=src_fd,  src_w=SRC_W, src_h=SRC_H,
                    src_fmt=RGA_FORMAT_YCbCr_420_SP,
                    dst_fd=disp_fd, dst_w=DISP_W, dst_h=DISP_H,
                    dst_fmt=RGA_FORMAT_RGB_888,
                )
            except RGAError as e:
                log.error(f"RGA 失败: {e}")
                rga_ok = False
            # 两路 RGA 均完成后归还摄像头 buffer
            cam.requeue(buf_idx)
            if not rga_ok:
                error_count += 1
                continue
            t_rga = (time.perf_counter() - t0) * 1000

            # ── NPU 推理 + 后处理（模式分支）──
            t0 = time.perf_counter()
            try:
                mm = mmap.mmap(npu_input_fd, npu_input_size, mmap.MAP_SHARED, mmap.PROT_READ)
                rgb_np = np.frombuffer(mm.read(npu_input_size),
                                       dtype=np.uint8).reshape(1, DST_H, DST_W, 3).copy()
                mm.close()
            except OSError as e:
                log.error(f"mmap npu_input_fd 失败: {e}，跳过本帧")
                error_count += 1
                continue
            t_npu = (time.perf_counter() - t0) * 1000

            # ── 携带模式：目标检测 ──
            detections = []
            if mode == 'carry':
                t0 = time.perf_counter()
                if detector is not None:
                    try:
                        detections = detector.process(
                            rgb_np[0], src_w=SRC_W, src_h=SRC_H,
                            content_w=CONTENT_W, content_h=CONTENT_H,
                            pad_left=PAD_LEFT, pad_top=PAD_TOP,
                            obstacle_only=True,
                        )
                    except Exception as e:
                        log.debug(f'目标检测异常: {e}')
                t_post = (time.perf_counter() - t0) * 1000

            # ── 放置模式：pose 推理 + 摔倒检测 + 人脸识别 ──
            boxes, confs, kps_list = [], [], []
            person_states = []
            identities = None
            if mode == 'place':
                try:
                    outputs = rknn.inference(inputs=[rgb_np])
                except Exception as e:
                    log.error(f"NPU 推理失败: {e}")
                    error_count += 1
                    continue

                t0 = time.perf_counter()
                try:
                    boxes, confs, kps_list = postprocess_trunc(
                        outputs, SRC_W, SRC_H,
                        scale=CONTENT_H / SRC_H,
                        pad_left=PAD_LEFT, pad_top=PAD_TOP
                    )
                except Exception as e:
                    log.warning(f"后处理异常: {e}")
                    boxes, confs, kps_list = [], [], []

                # 摔倒检测
                person_states = fall_detector.update(boxes, confs, kps_list)

                # 清理已消失 track_id 的缓存
                active_ids = {ps.track_id for ps in person_states}
                for tid in list(identity_cache.keys()):
                    if tid not in active_ids:
                        del identity_cache[tid]

                # 人脸识别（仅对未锁定身份的人体运行，已锁定的直接用缓存）
                if face_rec is not None and len(boxes) > 0:
                    try:
                        needs_recog = [
                            ps.track_id not in identity_cache or
                            identity_cache[ps.track_id][0] == UNKNOWN_LABEL
                            for ps in person_states
                        ]
                        if any(needs_recog):
                            with mmap.mmap(npu_input_fd, npu_input_size,
                                           mmap.MAP_SHARED, mmap.PROT_READ) as mm_face:
                                rgb_face = np.frombuffer(
                                    mm_face.read(npu_input_size), dtype=np.uint8
                                ).reshape(DST_H, DST_W, 3).copy()
                            bgr_face = cv2.cvtColor(rgb_face, cv2.COLOR_RGB2BGR)
                            scale_x = CONTENT_W / SRC_W
                            scale_y = CONTENT_H / SRC_H
                            boxes_lbox_all = [[
                                b[0] * scale_x + PAD_LEFT,
                                b[1] * scale_y + PAD_TOP,
                                b[2] * scale_x + PAD_LEFT,
                                b[3] * scale_y + PAD_TOP,
                            ] for b in boxes]
                            boxes_to_recog = [b for b, need in zip(boxes_lbox_all, needs_recog) if need]
                            raw_results = face_rec.process(bgr_face, boxes_to_recog)
                            raw_iter = iter(raw_results)
                            raw_identities = []
                            for need in needs_recog:
                                if need:
                                    raw_identities.append(next(raw_iter))
                                else:
                                    raw_identities.append((UNKNOWN_LABEL, -1.0, None))
                        else:
                            raw_identities = [(UNKNOWN_LABEL, -1.0, None)] * len(person_states)

                        identities_src = []
                        scale_x = CONTENT_W / SRC_W
                        scale_y = CONTENT_H / SRC_H
                        for ps, (name, score, fbox) in zip(person_states, raw_identities):
                            tid = ps.track_id
                            fbox_src = None
                            if fbox is not None:
                                fbox_src = [
                                    (fbox[0] - PAD_LEFT) / scale_x,
                                    (fbox[1] - PAD_TOP)  / scale_y,
                                    (fbox[2] - PAD_LEFT) / scale_x,
                                    (fbox[3] - PAD_TOP)  / scale_y,
                                ]
                            if name != UNKNOWN_LABEL and score >= MATCH_THRESH:
                                identity_cache[tid] = (name, score)
                            if tid in identity_cache:
                                cached_name, cached_score = identity_cache[tid]
                                identities_src.append((cached_name, cached_score, fbox_src))
                            else:
                                identities_src.append((name, score, fbox_src))
                        identities = identities_src
                    except Exception as e:
                        log.debug(f'人脸识别异常: {e}')

                t_post = (time.perf_counter() - t0) * 1000

            # ── 处理告警（仅放置模式）──
            for i, ps in enumerate(person_states):
                if ps.alert_triggered:
                    fall_count += 1
                    identity, id_score = UNKNOWN_LABEL, -1.0
                    if identities and i < len(identities):
                        identity, id_score, _ = identities[i]
                    _on_fall_alert(ps, frame_count, identity, id_score)
            t_post = (time.perf_counter() - t0) * 1000

            # ── 统计 ──
            fps = 1.0 / (time.perf_counter() - t_start + 1e-9)
            fps_list.append(fps);     t_v4l2_list.append(t_v4l2)
            t_rga_list.append(t_rga); t_npu_list.append(t_npu)
            t_post_list.append(t_post)
            for lst in (fps_list, t_v4l2_list, t_rga_list, t_npu_list, t_post_list):
                if len(lst) > 30: lst.pop(0)

            frame_count += 1

            if frame_count % 30 == 0:
                avg_fps   = sum(fps_list)    / len(fps_list)
                avg_v4l2  = sum(t_v4l2_list) / len(t_v4l2_list)
                avg_rga   = sum(t_rga_list)  / len(t_rga_list)
                avg_npu   = sum(t_npu_list)  / len(t_npu_list)
                avg_post  = sum(t_post_list) / len(t_post_list)
                mem_mb, cma_mb = _read_meminfo()
                temp_c = _read_temp_c()
                if mode == 'place':
                    fallen_now = fall_detector.alert_count
                    log.info(
                        f"帧{frame_count:6d}  FPS:{avg_fps:.1f}  "
                        f"v4l2:{avg_v4l2:.0f}ms rga:{avg_rga:.0f}ms "
                        f"npu:{avg_npu:.0f}ms post:{avg_post:.0f}ms  "
                        f"Persons:{len(boxes)} Fallen:{fallen_now} Alerts:{fall_count}  "
                        f"mem:{mem_mb}MB cma:{cma_mb}MB temp:{temp_c}°C err:{error_count}"
                    )
                else:
                    log.info(
                        f"帧{frame_count:6d}  FPS:{avg_fps:.1f}  "
                        f"v4l2:{avg_v4l2:.0f}ms rga:{avg_rga:.0f}ms "
                        f"npu:{avg_npu:.0f}ms post:{avg_post:.0f}ms  "
                        f"Detections:{len(detections)}  "
                        f"mem:{mem_mb}MB temp:{temp_c}°C err:{error_count}"
                    )

            if SHOW_DISPLAY:
                # ── 构建显示帧 ──
                try:
                    mm = mmap.mmap(disp_fd, disp_size, mmap.MAP_SHARED, mmap.PROT_READ)
                    disp_np = np.frombuffer(mm.read(disp_size),
                                            dtype=np.uint8).reshape(DISP_H, DISP_W, 3).copy()
                    mm.close()
                    disp = cv2.cvtColor(disp_np, cv2.COLOR_RGB2BGR)
                    sx_d = DISP_W / SRC_W
                    sy_d = DISP_H / SRC_H
                    if mode == 'place':
                        disp = draw_pose_scaled(disp, boxes, confs, kps_list, sx_d, sy_d)
                        disp = draw_fall_overlay(disp, person_states, sx_d, sy_d, identities)
                    else:
                        disp = draw_detections(disp, detections, sx_d, sy_d)
                except Exception as _disp_err:
                    log.warning(f'构建显示帧异常: {_disp_err}')
                    disp = np.zeros((DISP_H, DISP_W, 3), dtype=np.uint8)

                # ── HUD ──
                if mode == 'place':
                    fallen_now = fall_detector.alert_count
                    hud_color = (0, 0, 255) if fallen_now > 0 else (0, 255, 0)
                    hud_text  = (f'FPS:{avg_fps:.1f}  Persons:{len(boxes)}'
                                 f'  FALLEN:{fallen_now}  Alerts:{fall_count}')
                    cv2.putText(disp, hud_text, (10, 40),
                                cv2.FONT_HERSHEY_SIMPLEX, 1.0, hud_color, 2)
                    if fallen_now > 0:
                        flash = int(time.time() * 2) % 2 == 0
                        if flash:
                            cv2.rectangle(disp, (0, 0), (DISP_W - 1, DISP_H - 1),
                                          (0, 0, 255), 12)
                else:
                    hud_text = f'FPS:{avg_fps:.1f}  Obstacles:{len(detections)}  [Carry Mode]'
                    cv2.putText(disp, hud_text, (10, 40),
                                cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 200), 2)

                cv2.imshow(window_name, disp)
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break

            # ── 推流（无论是否有屏幕显示都推） ──
            try:
                if SHOW_DISPLAY and 'disp' in dir():
                    push_frame = disp
                else:
                    # 没有显示路时用推理路的小图
                    mm = mmap.mmap(npu_input_fd, npu_input_size,
                                   mmap.MAP_SHARED, mmap.PROT_READ)
                    rgb_small = np.frombuffer(
                        mm.read(npu_input_size), dtype=np.uint8
                    ).reshape(DST_H, DST_W, 3).copy()
                    mm.close()
                    push_frame = cv2.cvtColor(rgb_small, cv2.COLOR_RGB2BGR)

                if mode == 'place':
                    _push_frame(push_frame, {
                        'mode':       'place',
                        'fps':        round(avg_fps, 1),
                        'persons':    len(boxes),
                        'fallen':     fall_detector.alert_count,
                        'alerts':     fall_count,
                        'temp':       temp_c if frame_count % 30 == 0 else _stream.stats.get('temp', 0),
                        'mem_mb':     mem_mb  if frame_count % 30 == 0 else _stream.stats.get('mem_mb', 0),
                        'identities': [
                            {'name': n, 'score': round(s, 2)}
                            for n, s, _ in (identities or [])
                            if n != UNKNOWN_LABEL
                        ],
                    })
                    # 跌倒截图
                    for i, ps in enumerate(person_states):
                        if ps.alert_triggered:
                            identity, id_score = UNKNOWN_LABEL, -1.0
                            if identities and i < len(identities):
                                identity, id_score, _ = identities[i]
                            _save_fall_snapshot(push_frame, ps.track_id, identity, id_score)
                else:
                    _push_frame(push_frame, {
                        'mode':       'carry',
                        'fps':        round(avg_fps, 1),
                        'temp':       temp_c if frame_count % 30 == 0 else _stream.stats.get('temp', 0),
                        'mem_mb':     mem_mb  if frame_count % 30 == 0 else _stream.stats.get('mem_mb', 0),
                        'detections': [
                            {'cls': d['cls'], 'name': d['name'],
                             'name_zh': d['name_zh'], 'conf': round(d['conf'], 2)}
                            for d in detections
                        ],
                    })
            except Exception as e:
                log.debug(f'推流异常: {e}')

            # ── MQTT 周期上报视觉状态（放置模式，每 MQTT_REPORT_INTERVAL 秒一次）──
            if mode == 'place':
                now = time.time()
                if now - last_mqtt_report >= MQTT_REPORT_INTERVAL:
                    last_mqtt_report = now
                    # 整理当前帧识别到的人员
                    known = [
                        {'name': n, 'score': round(s, 2)}
                        for n, s, _ in (identities or [])
                        if n != UNKNOWN_LABEL
                    ]
                    stranger_count = sum(
                        1 for n, s, _ in (identities or [])
                        if n == UNKNOWN_LABEL
                    ) if identities else 0
                    _mqtt.publish(MQTT_TOPIC_VISION, {
                        'event':          'status',
                        'persons':        len(boxes) if 'boxes' in dir() else 0,
                        'known':          known,
                        'stranger_count': stranger_count,
                        'fallen':         fall_detector.alert_count if fall_detector else 0,
                        'fps':            round(avg_fps, 1),
                        'ts':             int(now),
                    })

            # ── 反诈骗监听：通知当前帧是否有陌生人（仅放置模式）──
            if antiscam is not None and identities is not None:
                has_stranger = any(n == UNKNOWN_LABEL for n, s, _ in identities)
                antiscam.notify_stranger(has_stranger)

    except KeyboardInterrupt:
        log.info("用户中断")

    # ── 清理 ──
    cam.close()
    rga.free_output_buffer(npu_input_fd)
    rga.free_output_buffer(disp_fd)
    rga.close()
    if rknn is not None:
        rknn.release()
    if face_rec is not None:
        face_rec.release()
    if detector is not None:
        detector.release()
    cv2.destroyAllWindows()
    log.info(f"退出，共处理 {frame_count} 帧")


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='Guardian 推理管线')
    parser.add_argument('--mode', choices=['place', 'carry'], default='place',
                        help='运行模式：place=放置（跌倒检测+人脸识别），carry=携带（目标检测）')
    args = parser.parse_args()
    main(mode=args.mode)
