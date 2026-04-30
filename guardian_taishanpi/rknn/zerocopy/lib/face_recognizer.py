"""
face_recognizer.py — Guardian 人脸检测 + 识别模块
===================================================

两个 RKNN 模型运行在同一个 NPU Core 1 上（串行），不干扰 Core 0 的 pose 模型。

模型：
  - SCRFD-500M-KPS：人脸检测，输出 bbox + 5关键点（用于对齐）
  - MobileNet ArcFace：人脸识别，输出 512 维特征向量

流程：
  1. detect(bgr_full, person_box)  — 在人体区域内检测人脸
  2. align(bgr_full, kps5)         — 5点仿射变换，裁剪标准 112×112 人脸
  3. extract(face_112)             — 提取 512 维特征向量
  4. match(feat)                   — 与人脸库余弦相似度匹配

人脸库文件：face_db.npz
  {'names': [...], 'features': (N, 512) float32}

模型文件路径（板子上）：
  /home/lckfb/rknn/scrfd_500m_kps.rknn
  /home/lckfb/rknn/mobilenet_arcface.rknn
"""

import os
import time
import logging
import numpy as np
import cv2

log = logging.getLogger(__name__)

# ── 模型路径 ────────────────────────────────────────────────────────
SCRFD_MODEL_PATH    = '/home/lckfb/rknn/scrfd_500m_kps.rknn'
ARCFACE_MODEL_PATH  = '/home/lckfb/rknn/mobilenet_arcface.rknn'
FACE_DB_PATH        = '/home/lckfb/rknn/face_db.npz'

# ── 检测参数 ─────────────────────────────────────────────────────────
SCRFD_INPUT_SIZE    = 640          # SCRFD 输入尺寸
SCRFD_CONF_THRESH   = 0.5
SCRFD_NMS_THRESH    = 0.4

# ── 识别参数 ─────────────────────────────────────────────────────────
ARCFACE_INPUT_SIZE  = 112          # ArcFace 输入尺寸
MATCH_THRESH        = 0.45         # 余弦相似度阈值，低于此值判定为"未知"
UNKNOWN_LABEL       = 'Unknown'

# ── ArcFace 人脸对齐：5 个标准关键点位置（112×112 空间）─────────────
# 顺序：左眼 右眼 鼻尖 左嘴角 右嘴角
_ARCFACE_DST_KPS = np.array([
    [38.2946, 51.6963],
    [73.5318, 51.5014],
    [56.0252, 71.7366],
    [41.5493, 92.3655],
    [70.7299, 92.2041],
], dtype=np.float32)


# ── SCRFD 后处理辅助 ─────────────────────────────────────────────────

def _scrfd_nms(boxes, scores, iou_thresh):
    if len(boxes) == 0:
        return []
    boxes  = np.array(boxes,  dtype=np.float32)
    scores = np.array(scores, dtype=np.float32)
    x1, y1, x2, y2 = boxes[:,0], boxes[:,1], boxes[:,2], boxes[:,3]
    areas  = (x2 - x1) * (y2 - y1)
    order  = scores.argsort()[::-1]
    keep   = []
    while order.size > 0:
        i = order[0]; keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        w   = np.maximum(0.0, xx2 - xx1)
        h   = np.maximum(0.0, yy2 - yy1)
        iou = w * h / (areas[i] + areas[order[1:]] - w * h + 1e-6)
        order = order[1:][iou < iou_thresh]
    return keep


def _scrfd_postprocess(outputs, input_w, input_h, scale_x, scale_y,
                        pad_left, pad_top, conf_thresh, nms_thresh):
    """
    SCRFD-500M-KPS 后处理（insightface buffalo_sc 实际输出格式）。

    输出共 9 个张量，顺序：
      [0] score_stride8  (12800, 1)  — stride=8,  80×80 grid × 2 anchors/cell
      [1] score_stride16 ( 3200, 1)  — stride=16, 40×40 grid × 2 anchors/cell
      [2] score_stride32 (  800, 1)  — stride=32, 20×20 grid × 2 anchors/cell
      [3] bbox_stride8   (12800, 4)
      [4] bbox_stride16  ( 3200, 4)
      [5] bbox_stride32  (  800, 4)
      [6] kps_stride8    (12800, 10)
      [7] kps_stride16   ( 3200, 10)
      [8] kps_stride32   (  800, 10)

    每格 2 个 anchor（num_anchors=2），bbox/kps 值已乘以 stride 归一化。
    """
    strides     = [8, 16, 32]
    num_anchors = 2          # SCRFD-500M 每格 2 个 anchor
    all_boxes   = []
    all_confs   = []
    all_kps5    = []

    for si, stride in enumerate(strides):
        # 按新顺序取张量
        score_raw = outputs[si]          # (N, 1)
        bbox_raw  = outputs[si + 3]      # (N, 4)
        kps_raw   = outputs[si + 6]      # (N, 10)

        scores = score_raw.reshape(-1)        # (N,)
        bboxes = bbox_raw.reshape(-1, 4)      # (N, 4)
        kpss   = kps_raw.reshape(-1, 5, 2)   # (N, 5, 2)

        grid_h = input_h // stride
        grid_w = input_w // stride

        # 生成 anchor 中心坐标（每格 num_anchors 个，坐标相同）
        cy, cx = np.meshgrid(
            (np.arange(grid_h) + 0.5) * stride,
            (np.arange(grid_w) + 0.5) * stride,
            indexing='ij'
        )
        # 每个格子重复 num_anchors 次
        centers = np.stack([cx.ravel(), cy.ravel()], axis=1)   # (H*W, 2)
        centers = np.repeat(centers, num_anchors, axis=0)       # (H*W*2, 2)

        mask = scores >= conf_thresh
        if mask.sum() == 0:
            continue

        scores_f  = scores[mask]
        bboxes_f  = bboxes[mask]
        kpss_f    = kpss[mask]
        centers_f = centers[mask]

        # bbox 解码：ltrb（stride 单位） → x1y1x2y2（input 坐标系）
        boxes_inp = np.stack([
            centers_f[:, 0] - bboxes_f[:, 0] * stride,
            centers_f[:, 1] - bboxes_f[:, 1] * stride,
            centers_f[:, 0] + bboxes_f[:, 2] * stride,
            centers_f[:, 1] + bboxes_f[:, 3] * stride,
        ], axis=1)

        # kps 解码（stride 单位偏移 → 绝对坐标）
        kps_inp = kpss_f * stride + centers_f[:, None, :]

        # letterbox 坐标 → 原图坐标
        boxes_inp[:, 0] = (boxes_inp[:, 0] - pad_left) / scale_x
        boxes_inp[:, 2] = (boxes_inp[:, 2] - pad_left) / scale_x
        boxes_inp[:, 1] = (boxes_inp[:, 1] - pad_top)  / scale_y
        boxes_inp[:, 3] = (boxes_inp[:, 3] - pad_top)  / scale_y
        kps_inp[:, :, 0] = (kps_inp[:, :, 0] - pad_left) / scale_x
        kps_inp[:, :, 1] = (kps_inp[:, :, 1] - pad_top)  / scale_y

        all_boxes.extend(boxes_inp.tolist())
        all_confs.extend(scores_f.tolist())
        all_kps5.extend(kps_inp.tolist())

    if not all_boxes:
        return [], [], []

    keep = _scrfd_nms(all_boxes, all_confs, nms_thresh)
    all_boxes = [all_boxes[i] for i in keep]
    all_confs = [all_confs[i] for i in keep]
    all_kps5  = [all_kps5[i]  for i in keep]
    return all_boxes, all_confs, all_kps5


def _align_face(bgr, kps5):
    """
    5点仿射变换，将人脸对齐到标准 112×112 坐标系。
    kps5: (5, 2) 原图坐标，顺序：左眼 右眼 鼻尖 左嘴角 右嘴角
    返回 112×112 BGR 图像，失败返回 None。
    """
    src = np.array(kps5, dtype=np.float32)
    dst = _ARCFACE_DST_KPS.copy()
    M, _ = cv2.estimateAffinePartial2D(src, dst,
                                        method=cv2.LMEDS,
                                        confidence=0.9)
    if M is None:
        return None
    face = cv2.warpAffine(bgr, M, (ARCFACE_INPUT_SIZE, ARCFACE_INPUT_SIZE),
                           flags=cv2.INTER_LINEAR)
    return face


def _cosine_similarity(a, b):
    """两个向量（或矩阵行向量）的余弦相似度"""
    a_norm = a / (np.linalg.norm(a, axis=-1, keepdims=True) + 1e-6)
    b_norm = b / (np.linalg.norm(b, axis=-1, keepdims=True) + 1e-6)
    return float(np.dot(a_norm, b_norm.T).max()) if b_norm.ndim == 2 else float(np.dot(a_norm, b_norm))


# ── 主类 ─────────────────────────────────────────────────────────────

class FaceRecognizer:
    """
    人脸检测 + 识别器。
    两个 RKNN 模型均绑定到 NPU_CORE_1，与 pose 模型（CORE_0）并行。

    使用方式：
        fr = FaceRecognizer()
        fr.load_models()          # 加载 RKNN 模型
        fr.load_db()              # 加载人脸库

        # 每帧调用：
        identities = fr.process(bgr_frame, boxes)
        # 返回 [(name, score), ...] 与 boxes 一一对应

        # 录入人脸：
        fr.enroll('张三', bgr_frame, boxes[0])
        fr.save_db()
    """

    def __init__(self,
                 scrfd_path=SCRFD_MODEL_PATH,
                 arcface_path=ARCFACE_MODEL_PATH,
                 db_path=FACE_DB_PATH):
        self._scrfd_path   = scrfd_path
        self._arcface_path = arcface_path
        self._db_path      = db_path

        self._rknn_det = None
        self._rknn_rec = None

        # 人脸库：{name: [feat_vec, ...]}
        self._db_names: list    = []    # (N,) 每条特征对应的姓名
        self._db_feats          = None  # (N, 512) float32，None 表示库为空

        self._loaded = False

    # ── 初始化 ────────────────────────────────────────────────────

    def load_models(self, npu_core=None):
        """
        加载两个 RKNN 模型，绑定到指定 NPU Core。
        npu_core: RKNNLite.NPU_CORE_1（默认）
        """
        try:
            from rknnlite.api import RKNNLite
        except ImportError:
            log.warning('RKNNLite 不可用，人脸模块将以纯 CPU 模式运行（仅用于开发调试）')
            self._loaded = False
            return False

        if npu_core is None:
            npu_core = RKNNLite.NPU_CORE_1

        # 检测模型
        self._rknn_det = RKNNLite()
        ret = self._rknn_det.load_rknn(self._scrfd_path)
        if ret != 0:
            log.error(f'加载 SCRFD 模型失败: {self._scrfd_path}')
            return False
        ret = self._rknn_det.init_runtime(core_mask=npu_core)
        if ret != 0:
            log.error('SCRFD init_runtime 失败')
            return False
        log.info(f'SCRFD 模型加载成功，NPU Core mask={npu_core}')

        # 识别模型（同一个 core，串行运行）
        self._rknn_rec = RKNNLite()
        ret = self._rknn_rec.load_rknn(self._arcface_path)
        if ret != 0:
            log.error(f'加载 ArcFace 模型失败: {self._arcface_path}')
            return False
        ret = self._rknn_rec.init_runtime(core_mask=npu_core)
        if ret != 0:
            log.error('ArcFace init_runtime 失败')
            return False
        log.info(f'ArcFace 模型加载成功，NPU Core mask={npu_core}')

        self._loaded = True
        return True

    def load_db(self, path=None):
        """从 .npz 文件加载人脸库"""
        p = path or self._db_path
        if not os.path.exists(p):
            log.info(f'人脸库文件不存在: {p}，从空库开始')
            return
        try:
            data = np.load(p, allow_pickle=True)
            self._db_names = list(data['names'])
            self._db_feats = data['features'].astype(np.float32)
            log.info(f'人脸库加载成功: {len(self._db_names)} 条记录，路径={p}')
        except Exception as e:
            log.error(f'加载人脸库失败: {e}')

    def save_db(self, path=None):
        """保存人脸库到 .npz 文件"""
        p = path or self._db_path
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if self._db_feats is None or len(self._db_names) == 0:
            log.warning('人脸库为空，跳过保存')
            return
        np.savez(p,
                 names=np.array(self._db_names),
                 features=self._db_feats)
        log.info(f'人脸库已保存: {len(self._db_names)} 条 → {p}')

    @property
    def is_loaded(self):
        return self._loaded

    @property
    def db_count(self):
        return len(self._db_names)

    def db_list(self):
        """返回人脸库中所有姓名（去重）"""
        return sorted(set(self._db_names))

    # ── 核心推理 ──────────────────────────────────────────────────

    def _detect_faces(self, bgr, person_box=None):
        """
        在 bgr 图像中检测人脸。
        person_box: [x1,y1,x2,y2] 如果提供，只在人体区域内检测（加速 + 减少误检）。
        返回 (boxes_list, kps5_list)，坐标均为原图空间。
        """
        if person_box is not None:
            x1, y1, x2, y2 = [int(v) for v in person_box]
            x1 = max(0, x1); y1 = max(0, y1)
            x2 = min(bgr.shape[1], x2); y2 = min(bgr.shape[0], y2)
            roi = bgr[y1:y2, x1:x2]
            roi_offset_x, roi_offset_y = x1, y1
        else:
            roi = bgr
            roi_offset_x, roi_offset_y = 0, 0

        h, w = roi.shape[:2]
        if h < 10 or w < 10:
            return [], []

        # Letterbox 缩放到 SCRFD_INPUT_SIZE
        scale = min(SCRFD_INPUT_SIZE / w, SCRFD_INPUT_SIZE / h)
        new_w = int(w * scale)
        new_h = int(h * scale)
        pad_left = (SCRFD_INPUT_SIZE - new_w) // 2
        pad_top  = (SCRFD_INPUT_SIZE - new_h) // 2

        resized = cv2.resize(roi, (new_w, new_h))
        inp = np.zeros((SCRFD_INPUT_SIZE, SCRFD_INPUT_SIZE, 3), dtype=np.uint8)
        inp[pad_top:pad_top+new_h, pad_left:pad_left+new_w] = resized
        # SCRFD 期望 RGB 输入
        inp_rgb = cv2.cvtColor(inp, cv2.COLOR_BGR2RGB)
        inp4d   = inp_rgb[np.newaxis]  # (1, 640, 640, 3)

        try:
            outputs = self._rknn_det.inference(inputs=[inp4d])
        except Exception as e:
            log.debug(f'SCRFD 推理失败: {e}')
            return [], []

        boxes, confs, kps5_list = _scrfd_postprocess(
            outputs,
            SCRFD_INPUT_SIZE, SCRFD_INPUT_SIZE,
            scale, scale,
            pad_left, pad_top,
            SCRFD_CONF_THRESH, SCRFD_NMS_THRESH
        )

        # 将坐标从 ROI 空间映射回原图空间
        out_boxes, out_kps5 = [], []
        for box, kps5 in zip(boxes, kps5_list):
            bx1 = box[0] + roi_offset_x
            by1 = box[1] + roi_offset_y
            bx2 = box[2] + roi_offset_x
            by2 = box[3] + roi_offset_y
            kps5_full = np.array(kps5, dtype=np.float32)
            kps5_full[:, 0] += roi_offset_x
            kps5_full[:, 1] += roi_offset_y
            out_boxes.append([bx1, by1, bx2, by2])
            out_kps5.append(kps5_full)

        return out_boxes, out_kps5

    def _extract_feature(self, face_112_bgr):
        """
        从 112×112 对齐人脸图像提取 512 维特征向量。
        返回 (512,) float32，失败返回 None。
        """
        # RKNN config 已设置 mean=127.5 std=127.5，推理时自动归一化
        # 这里直接传 uint8 原图（RGB），不要手动归一化（否则会双重归一化）
        face_rgb = cv2.cvtColor(face_112_bgr, cv2.COLOR_BGR2RGB)
        inp4d    = face_rgb[np.newaxis]  # (1, 112, 112, 3) uint8

        try:
            outputs = self._rknn_rec.inference(inputs=[inp4d])
        except Exception as e:
            log.debug(f'ArcFace 推理失败: {e}')
            return None

        feat = outputs[0].flatten().astype(np.float32)  # (512,)
        # L2 归一化
        norm = np.linalg.norm(feat) + 1e-6
        return feat / norm

    # ── 公共接口 ──────────────────────────────────────────────────

    def process(self, bgr_frame, person_boxes):
        """
        对每个人体检测框，尝试检测人脸并识别身份。

        参数:
            bgr_frame    : 原图（BGR）
            person_boxes : list of [x1,y1,x2,y2]（原图坐标，pose 模型输出）

        返回:
            identities: list of (name: str, score: float, face_box: list or None)
            与 person_boxes 一一对应。
            name=UNKNOWN_LABEL 表示未识别，score 为最高相似度（或 -1 表示未检测到脸）。
        """
        if not self._loaded:
            return [(UNKNOWN_LABEL, -1.0, None)] * len(person_boxes)

        identities = []
        for pbox in person_boxes:
            face_boxes, kps5_list = self._detect_faces(bgr_frame, pbox)

            if not face_boxes:
                identities.append((UNKNOWN_LABEL, -1.0, None))
                continue

            # 取检测到的最大人脸（面积最大）
            areas = [(b[2]-b[0])*(b[3]-b[1]) for b in face_boxes]
            best_idx = int(np.argmax(areas))
            face_box = face_boxes[best_idx]
            kps5     = kps5_list[best_idx]

            # 人脸对齐
            face_112 = _align_face(bgr_frame, kps5)
            if face_112 is None:
                identities.append((UNKNOWN_LABEL, -1.0, face_box))
                continue

            # 特征提取
            feat = self._extract_feature(face_112)
            if feat is None:
                identities.append((UNKNOWN_LABEL, -1.0, face_box))
                continue

            # 与人脸库匹配
            name, score = self._match(feat)
            identities.append((name, score, face_box))

        return identities

    def _match(self, feat):
        """
        余弦相似度匹配，返回 (name, score)。
        库为空或最高相似度 < MATCH_THRESH 时返回 (UNKNOWN_LABEL, score)。
        """
        if self._db_feats is None or len(self._db_names) == 0:
            return UNKNOWN_LABEL, -1.0

        # 矩阵化批量相似度计算
        sims = np.dot(self._db_feats, feat)   # (N,)，已 L2 归一化，直接点积 = 余弦
        best_idx = int(np.argmax(sims))
        best_score = float(sims[best_idx])

        if best_score >= MATCH_THRESH:
            return self._db_names[best_idx], best_score
        return UNKNOWN_LABEL, best_score

    def enroll(self, name, bgr_frame, person_box=None):
        """
        录入人脸。

        参数:
            name        : 姓名字符串
            bgr_frame   : 原图（BGR）
            person_box  : 可选，人体区域 [x1,y1,x2,y2]，加速检测

        返回:
            True  — 录入成功
            False — 未检测到人脸或特征提取失败
        """
        if not self._loaded:
            log.error('模型未加载，无法录入')
            return False

        face_boxes, kps5_list = self._detect_faces(bgr_frame, person_box)
        if not face_boxes:
            log.warning(f'录入失败：图像中未检测到人脸 (name={name})')
            return False

        # 取最大人脸
        areas    = [(b[2]-b[0])*(b[3]-b[1]) for b in face_boxes]
        best_idx = int(np.argmax(areas))
        kps5     = kps5_list[best_idx]

        face_112 = _align_face(bgr_frame, kps5)
        if face_112 is None:
            log.warning(f'录入失败：人脸对齐失败 (name={name})')
            return False

        feat = self._extract_feature(face_112)
        if feat is None:
            log.warning(f'录入失败：特征提取失败 (name={name})')
            return False

        # 追加到人脸库
        self._db_names.append(name)
        if self._db_feats is None:
            self._db_feats = feat[np.newaxis]   # (1, 512)
        else:
            self._db_feats = np.vstack([self._db_feats, feat[np.newaxis]])

        log.info(f'录入成功: {name}，当前库共 {len(self._db_names)} 条')
        return True

    def delete(self, name):
        """删除人脸库中指定姓名的所有记录，返回删除条数"""
        if not self._db_names:
            return 0
        keep_mask = np.array([n != name for n in self._db_names])
        deleted   = int((~keep_mask).sum())
        self._db_names = [n for n, k in zip(self._db_names, keep_mask) if k]
        if self._db_feats is not None and deleted > 0:
            self._db_feats = self._db_feats[keep_mask] if keep_mask.any() else None
        log.info(f'删除 {name}: {deleted} 条，剩余 {len(self._db_names)} 条')
        return deleted

    def release(self):
        """释放 RKNN 资源"""
        if self._rknn_det:
            try:
                self._rknn_det.release()
            except Exception:
                pass
        if self._rknn_rec:
            try:
                self._rknn_rec.release()
            except Exception:
                pass
        self._loaded = False
        log.info('FaceRecognizer 已释放')
