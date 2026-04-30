"""
Guardian 声纹识别服务
=====================
提供两个接口供 xiaozhi-esp32-server 调用：
  GET  /voiceprint/health?key=<API_KEY>
  POST /voiceprint/identify?key=<API_KEY>
       form-data: speaker_ids=owner, file=audio.wav

工作原理：
  - 使用 resemblyzer 将音频转为 256 维声纹嵌入向量
  - 与已注册说话人的声纹向量做余弦相似度比较
  - 相似度 >= threshold → 识别为对应说话人
  - 相似度 <  threshold → 返回 "stranger"（陌生人）
"""

import os
import io
import json
import logging
import tempfile
import subprocess
import numpy as np

from pathlib import Path
from fastapi import FastAPI, UploadFile, File, Form, Query, HTTPException
from fastapi.responses import JSONResponse

# ── resemblyzer ──────────────────────────────────────────────
# 只导入 VoiceEncoder，不用 preprocess_wav（会触发 librosa/lzma 问题）
from resemblyzer import VoiceEncoder

# ── 配置 ─────────────────────────────────────────────────────
API_KEY            = os.environ.get("VOICEPRINT_API_KEY", "guardian_vp_key")
SPEAKERS_DIR       = Path(__file__).parent / "speakers"        # 声纹向量存放目录
SIMILARITY_THRESHOLD = float(os.environ.get("VP_THRESHOLD", "0.5"))
PORT               = int(os.environ.get("VP_PORT", "8002"))

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("voiceprint")

# ── 加载模型（启动时执行一次）────────────────────────────────
log.info("正在加载 resemblyzer 声纹模型...")
encoder = VoiceEncoder()
log.info("声纹模型加载完成")

app = FastAPI(title="Guardian Voiceprint Service", version="1.0.0")


# ── 工具函数 ─────────────────────────────────────────────────

def _check_key(key: str):
    if key != API_KEY:
        raise HTTPException(status_code=401, detail="Invalid API key")


def _embed_path() -> Path:
    """声纹向量文件路径（JSON格式存储）"""
    return SPEAKERS_DIR / "embeddings.json"


def _load_embeddings() -> dict:
    """加载所有已注册说话人的声纹向量"""
    path = _embed_path()
    if not path.exists():
        return {}
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    # 将 list 还原为 numpy array
    return {k: np.array(v) for k, v in data.items()}


def _save_embeddings(embeddings: dict):
    """保存声纹向量到文件"""
    SPEAKERS_DIR.mkdir(parents=True, exist_ok=True)
    data = {k: v.tolist() for k, v in embeddings.items()}
    with open(_embed_path(), "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)


def _cosine_similarity(a: np.ndarray, b: np.ndarray) -> float:
    """计算两个向量的余弦相似度"""
    return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-8))


def _audio_to_embedding(audio_bytes: bytes) -> np.ndarray:
    """
    将音频字节转为 256 维声纹嵌入向量。
    完全绕过 resemblyzer.preprocess_wav（内部用 librosa，在 Windows 上触发 lzma DLL 缺失）。
    改用 soundfile 读取 + 手动重采样到 16kHz 单声道。
    支持 WAV / WebM / OGG（浏览器 MediaRecorder 输出格式）。
    """
    import soundfile as sf
    from scipy.signal import resample_poly
    from math import gcd

    # 检测格式头，写临时文件
    suffix = ".wav"
    if audio_bytes[:4] == b'\x1aE\xdf\xa3':
        suffix = ".webm"
    elif audio_bytes[:4] == b'OggS':
        suffix = ".ogg"

    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
        tmp.write(audio_bytes)
        tmp_path = tmp.name

    try:
        # soundfile 不支持 webm，需用 ffmpeg 转 wav
        if suffix in (".webm", ".ogg"):
            wav_path = tmp_path + ".wav"
            try:
                result = subprocess.run(
                    ["ffmpeg", "-y", "-i", tmp_path, "-ar", "16000", "-ac", "1", wav_path],
                    capture_output=True, timeout=15
                )
                if result.returncode != 0:
                    raise RuntimeError(f"ffmpeg 转换失败: {result.stderr.decode(errors='replace')}")
                data, sr = sf.read(wav_path)
            finally:
                if os.path.exists(wav_path):
                    os.unlink(wav_path)
        else:
            data, sr = sf.read(tmp_path)
    finally:
        os.unlink(tmp_path)

    # 转单声道
    if data.ndim > 1:
        data = data.mean(axis=1)

    # 重采样到 16000 Hz
    target_sr = 16000
    if sr != target_sr:
        g = gcd(sr, target_sr)
        data = resample_poly(data, target_sr // g, sr // g)

    # 转 float32，resemblyzer 要求
    wav = data.astype(np.float32)

    # 有效语音检测：RMS 能量过低说明是静音/按键噪声，直接拒绝
    MIN_DURATION_SEC = 1.0   # 至少 1 秒
    MIN_RMS          = 0.004 # 经验值，静音约 0.0001，正常说话约 0.02~0.1
    duration = len(wav) / 16000
    rms = float(np.sqrt(np.mean(wav ** 2)))
    log.info(f"音频时长: {duration:.2f}s, RMS能量: {rms:.5f}")
    if duration < MIN_DURATION_SEC:
        raise ValueError(f"音频太短 ({duration:.2f}s < {MIN_DURATION_SEC}s)，请说话后再提交")
    if rms < MIN_RMS:
        raise ValueError(f"音频能量过低 (RMS={rms:.5f})，检测为静音，请在安静环境正常说话")

    # 手动做 resemblyzer preprocess_wav 的归一化（幅度归一化）
    if wav.max() > 1.0:
        wav = wav / (wav.max() + 1e-8)

    embedding = encoder.embed_utterance(wav)
    return embedding


# ── 接口 ─────────────────────────────────────────────────────

@app.get("/voiceprint/health")
def health_check(key: str = Query(...)):
    """健康检查接口"""
    _check_key(key)
    embeddings = _load_embeddings()
    return {
        "status": "healthy",
        "registered_speakers": list(embeddings.keys()),
        "threshold": SIMILARITY_THRESHOLD,
    }


@app.post("/voiceprint/identify")
async def identify_speaker(
    key: str = Query(...),
    speaker_ids: str = Form(...),
    file: UploadFile = File(...),
):
    """
    声纹识别接口
    - speaker_ids: 逗号分隔的说话人ID列表（如 "owner"）
    - file: WAV 音频文件
    返回：{"speaker_id": "owner"|"stranger", "score": 0.85}
    """
    _check_key(key)

    audio_bytes = await file.read()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="Empty audio file")

    # 提取声纹向量
    try:
        query_embedding = _audio_to_embedding(audio_bytes)
    except Exception as e:
        log.error(f"声纹提取失败: {e}")
        raise HTTPException(status_code=422, detail=f"Audio processing failed: {e}")

    # 加载已注册声纹
    embeddings = _load_embeddings()
    if not embeddings:
        log.warning("没有已注册的说话人，返回陌生人")
        return JSONResponse({"speaker_id": "stranger", "score": 0.0})

    # 在请求的 speaker_ids 范围内查找最佳匹配
    requested_ids = [s.strip() for s in speaker_ids.split(",") if s.strip()]
    best_id = None
    best_score = -1.0

    for sid in requested_ids:
        if sid not in embeddings:
            log.warning(f"说话人 '{sid}' 未注册，跳过")
            continue
        score = _cosine_similarity(query_embedding, embeddings[sid])
        log.info(f"与 '{sid}' 的相似度: {score:.3f}")
        if score > best_score:
            best_score = score
            best_id = sid

    if best_id is None:
        return JSONResponse({"speaker_id": "stranger", "score": 0.0})

    if best_score >= SIMILARITY_THRESHOLD:
        log.info(f"识别成功: {best_id} (相似度={best_score:.3f})")
        return JSONResponse({"speaker_id": best_id, "score": best_score})
    else:
        log.warning(f"相似度 {best_score:.3f} 低于阈值 {SIMILARITY_THRESHOLD}，判定为陌生人")
        return JSONResponse({"speaker_id": "stranger", "score": best_score})


@app.post("/voiceprint/register")
async def register_speaker(
    key: str = Query(...),
    speaker_id: str = Form(...),
    file: UploadFile = File(...),
    accumulate: bool = Form(False),
):
    """
    声纹注册接口（管理用）
    - speaker_id: 说话人ID（如 "owner"）
    - file: WAV 音频文件（建议 10-30 秒干净人声）
    - accumulate: 若为 true，与已有声纹取平均（多次注册增强准确率）；默认 false（覆盖）
    """
    _check_key(key)

    audio_bytes = await file.read()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="Empty audio file")

    try:
        new_embedding = _audio_to_embedding(audio_bytes)
    except Exception as e:
        log.error(f"声纹提取失败: {e}")
        raise HTTPException(status_code=422, detail=f"Audio processing failed: {e}")

    embeddings = _load_embeddings()

    if accumulate and speaker_id in embeddings:
        # 与已有声纹向量取均值，使声纹更鲁棒
        old_embedding = embeddings[speaker_id]
        merged = (old_embedding + new_embedding) / 2.0
        # 归一化保持单位向量
        norm = np.linalg.norm(merged)
        embeddings[speaker_id] = merged / (norm + 1e-8)
        log.info(f"声纹累积更新成功: speaker_id='{speaker_id}'")
        action = "accumulated"
    else:
        embeddings[speaker_id] = new_embedding
        log.info(f"声纹注册成功: speaker_id='{speaker_id}'")
        action = "registered"

    _save_embeddings(embeddings)
    return {"status": "ok", "speaker_id": speaker_id, "action": action, "embedding_dim": len(embeddings[speaker_id])}


@app.get("/voiceprint/list")
def list_speakers(key: str = Query(...)):
    """列出所有已注册说话人"""
    _check_key(key)
    embeddings = _load_embeddings()
    return {"speakers": list(embeddings.keys())}


@app.delete("/voiceprint/delete")
def delete_speaker(key: str = Query(...), speaker_id: str = Query(...)):
    """删除已注册说话人"""
    _check_key(key)
    embeddings = _load_embeddings()
    if speaker_id not in embeddings:
        raise HTTPException(status_code=404, detail=f"Speaker '{speaker_id}' not found")
    del embeddings[speaker_id]
    _save_embeddings(embeddings)
    log.info(f"已删除说话人: {speaker_id}")
    return {"status": "ok", "deleted": speaker_id}


# ── 启动 ─────────────────────────────────────────────────────
if __name__ == "__main__":
    import uvicorn
    log.info(f"声纹识别服务启动，端口 {PORT}，阈值 {SIMILARITY_THRESHOLD}")
    log.info(f"声纹存储目录: {SPEAKERS_DIR.absolute()}")
    uvicorn.run(app, host="0.0.0.0", port=PORT)
