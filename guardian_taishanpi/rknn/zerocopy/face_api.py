"""
face_api.py — Guardian 人脸库管理 HTTP API
==========================================
在泰山派上运行，供 Web 前端调用，实现：
  GET  /face_db            — 查询人脸库列表
  POST /enroll             — 上传图片 + 姓名，录入人脸
  DELETE /face_db/{name}   — 删除指定姓名

启动方式：
  source ~/rknn/venv/bin/activate
  cd /home/lckfb/rknn/zerocopy
  python3 face_api.py

默认监听 0.0.0.0:8081
"""

import os
import sys
import logging
import tempfile
from pathlib import Path

import numpy as np
import cv2
from fastapi import FastAPI, UploadFile, File, Form, HTTPException
from fastapi.responses import JSONResponse
from fastapi.middleware.cors import CORSMiddleware
import uvicorn

# 把 zerocopy 父目录加入路径
sys.path.insert(0, str(Path(__file__).parent.parent))

from zerocopy.lib.face_recognizer import FaceRecognizer, FACE_DB_PATH

# ── 配置 ──────────────────────────────────────────────────────────────
PORT         = int(os.environ.get("FACE_API_PORT", "8081"))
SCRFD_PATH   = os.environ.get("SCRFD_PATH",   "/home/lckfb/rknn/scrfd_500m_kps.rknn")
ARCFACE_PATH = os.environ.get("ARCFACE_PATH", "/home/lckfb/rknn/mobilenet_arcface.rknn")
DB_PATH      = os.environ.get("FACE_DB_PATH", FACE_DB_PATH)

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("face_api")

# ── FastAPI ───────────────────────────────────────────────────────────
app = FastAPI(title="Guardian Face API", version="1.0.0")

# 允许前端跨域请求
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── 识别器（延迟加载，首次调用 /enroll 时才初始化 RKNN 模型）──────────
_fr: FaceRecognizer = None


def _get_recognizer() -> FaceRecognizer:
    """获取识别器实例，首次调用时加载模型"""
    global _fr
    if _fr is None:
        log.info("首次调用，正在加载 RKNN 模型...")
        _fr = FaceRecognizer(
            scrfd_path=SCRFD_PATH,
            arcface_path=ARCFACE_PATH,
            db_path=DB_PATH,
        )
        ok = _fr.load_models()
        if not ok:
            _fr = None
            raise RuntimeError("RKNN 模型加载失败，请检查模型文件路径")
        _fr.load_db()
        log.info("模型加载完成")
    return _fr


# ── 接口 ──────────────────────────────────────────────────────────────

@app.get("/face_db")
def list_faces():
    """
    查询人脸库列表。
    返回：{"total": N, "persons": [{"name": "张三", "count": 2}, ...]}
    不需要加载 RKNN 模型，直接读 npz 文件。
    """
    if not os.path.exists(DB_PATH):
        return {"total": 0, "persons": []}

    try:
        data = np.load(DB_PATH, allow_pickle=True)
        names = list(data["names"])
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"读取人脸库失败: {e}")

    from collections import Counter
    counts = Counter(names)
    persons = [{"name": n, "count": c} for n, c in sorted(counts.items())]
    return {"total": len(names), "persons": persons}


@app.post("/enroll")
async def enroll_face(
    name: str = Form(...),
    file: UploadFile = File(...),
):
    """
    录入人脸。
    - name : 姓名（中文/英文均可）
    - file : 图片文件（JPG/PNG），建议正面照，光线充足

    返回：{"status": "ok", "name": "张三", "total": 3}
    """
    if not name.strip():
        raise HTTPException(status_code=400, detail="姓名不能为空")

    # 读取上传图片
    img_bytes = await file.read()
    if not img_bytes:
        raise HTTPException(status_code=400, detail="图片文件为空")

    # 解码图片
    nparr = np.frombuffer(img_bytes, np.uint8)
    bgr = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    if bgr is None:
        raise HTTPException(status_code=422, detail="无法解码图片，请上传 JPG/PNG 格式")

    log.info(f"收到录入请求: name={name}, 图片尺寸={bgr.shape[1]}x{bgr.shape[0]}")

    # 获取识别器（加载模型）
    try:
        fr = _get_recognizer()
    except RuntimeError as e:
        raise HTTPException(status_code=503, detail=str(e))

    # 执行录入
    success = fr.enroll(name.strip(), bgr)
    if not success:
        raise HTTPException(
            status_code=422,
            detail="未在图片中检测到清晰人脸，请上传正面照片（光线充足、人脸居中）"
        )

    # 保存人脸库
    fr.save_db()

    log.info(f"录入成功: {name}，当前库共 {fr.db_count} 条")
    return {"status": "ok", "name": name.strip(), "total": fr.db_count}


@app.delete("/face_db/{name}")
def delete_face(name: str):
    """
    删除指定姓名的所有人脸记录。
    返回：{"status": "ok", "deleted": 2, "total": 1}
    """
    if not os.path.exists(DB_PATH):
        raise HTTPException(status_code=404, detail="人脸库为空")

    # 直接操作 npz，不需要加载 RKNN 模型
    try:
        data = np.load(DB_PATH, allow_pickle=True)
        names    = list(data["names"])
        features = data["features"]
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"读取人脸库失败: {e}")

    keep_mask = np.array([n != name for n in names])
    deleted   = int((~keep_mask).sum())

    if deleted == 0:
        raise HTTPException(status_code=404, detail=f"未找到姓名 '{name}' 的记录")

    new_names    = [n for n, k in zip(names, keep_mask) if k]
    new_features = features[keep_mask] if keep_mask.any() else np.zeros((0, features.shape[1]), dtype=np.float32)

    os.makedirs(os.path.dirname(DB_PATH), exist_ok=True)
    np.savez(DB_PATH, names=np.array(new_names), features=new_features)

    # 同步更新内存中的识别器（如果已初始化）
    if _fr is not None:
        _fr.load_db()

    log.info(f"删除 '{name}' 共 {deleted} 条，剩余 {len(new_names)} 条")
    return {"status": "ok", "deleted": deleted, "total": len(new_names)}


@app.get("/health")
def health():
    return {"status": "ok", "db_path": DB_PATH,
            "model_loaded": _fr is not None and _fr.is_loaded}


# ── 启动 ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    log.info(f"Guardian Face API 启动，端口 {PORT}")
    log.info(f"人脸库路径: {DB_PATH}")
    log.info(f"SCRFD 模型: {SCRFD_PATH}")
    log.info(f"ArcFace 模型: {ARCFACE_PATH}")
    uvicorn.run(app, host="0.0.0.0", port=PORT)
