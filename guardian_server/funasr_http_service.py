"""
funasr_http_service.py — 独立的 FunASR HTTP 识别服务
=====================================================
供泰山派反诈骗模块调用，独立于 xiaozhi-server 运行，端口 10097。

接口：
  POST /asr
    Content-Type: audio/wav
    Body: WAV 文件字节（16kHz, mono, int16）
    返回: {"text": "识别结果"}

启动方式：
  python funasr_http_service.py

依赖：funasr（已在 xiaozhi-esp32-server 环境中安装）
"""

import os
import io
import sys
import json
import wave
import logging
import tempfile
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s %(levelname)s %(message)s',
    datefmt='%H:%M:%S',
)
log = logging.getLogger(__name__)

# ── 配置 ──────────────────────────────────────────────────────
PORT = 10097
# SenseVoiceSmall 模型目录（相对于 xiaozhi-server 根目录）
_THIS_DIR   = os.path.dirname(os.path.abspath(__file__))
MODEL_DIR   = os.path.join(_THIS_DIR, 'main', 'xiaozhi-server', 'models', 'SenseVoiceSmall')
OUTPUT_DIR  = os.path.join(_THIS_DIR, 'main', 'xiaozhi-server', 'tmp')

os.makedirs(OUTPUT_DIR, exist_ok=True)

# ── FunASR 模型加载（启动时只加载一次）─────────────────────────
log.info(f'正在加载 FunASR 模型: {MODEL_DIR}')
try:
    from funasr import AutoModel
    _model = AutoModel(
        model=MODEL_DIR,
        vad_kwargs={"max_single_segment_time": 30000},
        disable_update=True,
        hub="hf",
    )
    log.info('FunASR 模型加载完成')
except Exception as e:
    log.error(f'FunASR 模型加载失败: {e}')
    sys.exit(1)


class _ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True


class _ASRHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass   # 关闭访问日志

    def do_GET(self):
        if self.path == '/health':
            self._json({'status': 'ok', 'model': MODEL_DIR})
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path != '/asr':
            self.send_response(404)
            self.end_headers()
            return

        ctype   = self.headers.get('Content-Type', '')
        length  = int(self.headers.get('Content-Length', 0))
        if length == 0:
            self._json({'error': '空请求体'}, 400)
            return

        wav_bytes = self.rfile.read(length)

        # 验证 WAV 头，提取 PCM 数据
        try:
            with wave.open(io.BytesIO(wav_bytes), 'rb') as wf:
                n_channels  = wf.getnchannels()
                sample_rate = wf.getframerate()
                n_frames    = wf.getnframes()
                pcm_bytes   = wf.readframes(n_frames)
        except Exception as e:
            self._json({'error': f'WAV 解析失败: {e}'}, 422)
            return

        log.info(f'[ASR] 收到音频: {sample_rate}Hz {n_channels}ch {n_frames}帧 ({len(pcm_bytes)} bytes PCM)')

        # 调用 FunASR 识别
        try:
            result = _model.generate(
                input=pcm_bytes,
                cache={},
                language="auto",
                use_itn=True,
                batch_size_s=60,
            )
            raw_text = result[0]['text'] if result else ''
            # 去除语言标签（如 <|zh|><|NEUTRAL|><|Speech|>）
            import re
            text = re.sub(r'<\|[^|]+\|>', '', raw_text).strip()
            log.info(f'[ASR] 识别结果: {text}')
            self._json({'text': text})
        except Exception as e:
            log.error(f'[ASR] 识别失败: {e}')
            self._json({'error': str(e)}, 500)

    def _json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(body)


def main():
    server = _ThreadingHTTPServer(('0.0.0.0', PORT), _ASRHandler)
    log.info(f'FunASR HTTP 服务启动: http://0.0.0.0:{PORT}/asr')
    log.info('等待请求...')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info('服务停止')


if __name__ == '__main__':
    main()
