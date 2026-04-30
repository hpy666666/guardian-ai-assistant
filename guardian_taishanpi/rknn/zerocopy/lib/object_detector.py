"""
object_detector.py — YOLOv8n-det 目标检测封装（携带模式）
==========================================================
职责：
  1. 加载 yolov8n_det_int8.rknn 并绑定 NPU Core 2
  2. 后处理：解析 (1, 84, 8400) 输出 → NMS → 映射回 SRC 坐标
  3. 过滤出"障碍物优先"类别，供 pipeline_carry.py 使用

COCO 80类 class id（0-indexed）：
  0  person          1  bicycle        2  car
  3  motorcycle      5  bus            7  truck
  9  traffic light   11 stop sign      24 backpack
  56 chair           57 couch          58 potted plant
  59 bed             60 dining table   62 tv
  63 laptop          66 keyboard       67 cell phone
  72 refrigerator    73 book           75 vase
  76 scissors        77 teddy bear     79 toothbrush
"""

import os
import numpy as np

# COCO 80类别名称（0-indexed）
COCO_CLASSES = [
    'person', 'bicycle', 'car', 'motorcycle', 'airplane', 'bus', 'train',
    'truck', 'boat', 'traffic light', 'fire hydrant', 'stop sign',
    'parking meter', 'bench', 'bird', 'cat', 'dog', 'horse', 'sheep', 'cow',
    'elephant', 'bear', 'zebra', 'giraffe', 'backpack', 'umbrella',
    'handbag', 'tie', 'suitcase', 'frisbee', 'skis', 'snowboard',
    'sports ball', 'kite', 'baseball bat', 'baseball glove', 'skateboard',
    'surfboard', 'tennis racket', 'bottle', 'wine glass', 'cup', 'fork',
    'knife', 'spoon', 'bowl', 'banana', 'apple', 'sandwich', 'orange',
    'broccoli', 'carrot', 'hot dog', 'pizza', 'donut', 'cake', 'chair',
    'couch', 'potted plant', 'bed', 'dining table', 'toilet', 'tv',
    'laptop', 'mouse', 'remote', 'keyboard', 'cell phone', 'microwave',
    'oven', 'toaster', 'sink', 'refrigerator', 'book', 'clock', 'vase',
    'scissors', 'teddy bear', 'hair drier', 'toothbrush',
]

# 障碍物优先类别（携带模式重点关注）
# 包含：行人、车辆、非机动车、交通标志、常见室内障碍物
OBSTACLE_CLASS_IDS = frozenset([
    0,   # person
    1,   # bicycle
    2,   # car
    3,   # motorcycle
    5,   # bus
    7,   # truck
    9,   # traffic light
    11,  # stop sign
    56,  # chair
    57,  # couch
    58,  # potted plant
    59,  # bed
    60,  # dining table
    63,  # laptop（地上的障碍）
])

# 中文类别名（用于画面标注和语音播报）
COCO_CLASSES_ZH = {
    0:  '行人',
    1:  '自行车',
    2:  '汽车',
    3:  '摩托车',
    5:  '公交车',
    7:  '卡车',
    9:  '红绿灯',
    11: '停车标志',
    56: '椅子',
    57: '沙发',
    58: '盆栽',
    59: '床',
    60: '餐桌',
    63: '笔记本',
}

# 默认模型路径
DEFAULT_MODEL_PATH = '/home/lckfb/rknn/yolov8n_det_fp16.rknn'

CONF_THRESH = 0.40   # fp16 模型正常阈值（实测最高分 ~0.68）
NMS_THRESH  = 0.45


def _nms(boxes, scores, iou_thresh):
    """非极大值抑制"""
    x1, y1, x2, y2 = boxes[:, 0], boxes[:, 1], boxes[:, 2], boxes[:, 3]
    areas = (x2 - x1) * (y2 - y1)
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        i = order[0]
        keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        w = np.maximum(0, xx2 - xx1)
        h = np.maximum(0, yy2 - yy1)
        iou = w * h / (areas[i] + areas[order[1:]] - w * h + 1e-6)
        order = order[1:][iou < iou_thresh]
    return keep


def postprocess_det(outputs, src_w, src_h,
                    dst_w=640, dst_h=640,
                    content_w=640, content_h=360,
                    pad_left=0, pad_top=140,
                    obstacle_only=True):
    """
    解析 YOLOv8n-det 模型输出。

    YOLOv8 det head 输出形状：(1, 84, 8400)
      - 84 = 4（xywh） + 80（类别 logit，RKNN 已做 sigmoid）
      - 8400 = 3个检测头的 anchor 数之和（80*80 + 40*40 + 20*20）

    返回：
        List[dict]，每个元素：
          {
            'box':   [x1, y1, x2, y2],  # SRC 坐标系（3840×2160）
            'conf':  float,
            'cls':   int,                # COCO class id
            'name':  str,                # 英文类别名
            'name_zh': str,              # 中文类别名（仅障碍物类别有）
          }
    """
    if outputs is None or len(outputs) == 0:
        return []

    # 支持两种常见输出格式
    raw = outputs[0]
    if raw.ndim == 3:
        pred = raw[0]          # (84, 8400)
    elif raw.ndim == 2:
        pred = raw             # (84, 8400) 已去掉 batch 维
    else:
        return []

    if pred.shape[0] == 84 and pred.shape[1] > pred.shape[0]:
        # 正常格式：(84, 8400)
        pass
    elif pred.shape[1] == 84:
        # 转置格式：(8400, 84)
        pred = pred.T
    else:
        return []

    # xywh（letterbox 坐标）
    boxes_xywh = pred[:4, :].T   # (8400, 4)
    # 类别置信度（已经过 sigmoid）
    cls_scores  = pred[4:, :].T  # (8400, 80)

    # 各候选框最高置信度类别
    cls_ids   = cls_scores.argmax(axis=1)          # (8400,)
    cls_confs = cls_scores[np.arange(len(cls_ids)), cls_ids]  # (8400,)

    # 过滤低置信度
    mask = cls_confs > CONF_THRESH
    if mask.sum() == 0:
        return []

    # 限制候选数量避免后处理太慢
    if mask.sum() > 200:
        top_idx = np.argsort(cls_confs[mask])[-200:]
        tmp = np.where(mask)[0][top_idx]
        mask = np.zeros(len(mask), dtype=bool)
        mask[tmp] = True

    bxywh  = boxes_xywh[mask]
    confs  = cls_confs[mask]
    ids    = cls_ids[mask]

    # 障碍物过滤（在 NMS 前过滤，节省计算）
    if obstacle_only:
        obs_mask = np.array([c in OBSTACLE_CLASS_IDS for c in ids])
        if obs_mask.sum() == 0:
            return []
        bxywh = bxywh[obs_mask]
        confs = confs[obs_mask]
        ids   = ids[obs_mask]

    # xywh → xyxy（letterbox 坐标）
    boxes = np.zeros_like(bxywh)
    boxes[:, 0] = bxywh[:, 0] - bxywh[:, 2] / 2   # x1
    boxes[:, 1] = bxywh[:, 1] - bxywh[:, 3] / 2   # y1
    boxes[:, 2] = bxywh[:, 0] + bxywh[:, 2] / 2   # x2
    boxes[:, 3] = bxywh[:, 1] + bxywh[:, 3] / 2   # y2

    # letterbox → SRC 坐标
    sx = src_w / content_w   # 3840/640 = 6.0
    sy = src_h / content_h   # 2160/360 = 6.0
    boxes[:, 0] = (boxes[:, 0] - pad_left) * sx
    boxes[:, 2] = (boxes[:, 2] - pad_left) * sx
    boxes[:, 1] = (boxes[:, 1] - pad_top)  * sy
    boxes[:, 3] = (boxes[:, 3] - pad_top)  * sy

    # 裁剪到图像范围
    boxes[:, 0] = np.clip(boxes[:, 0], 0, src_w)
    boxes[:, 2] = np.clip(boxes[:, 2], 0, src_w)
    boxes[:, 1] = np.clip(boxes[:, 1], 0, src_h)
    boxes[:, 3] = np.clip(boxes[:, 3], 0, src_h)

    # 过滤无效框
    valid = ((boxes[:, 2] > boxes[:, 0]) &
             (boxes[:, 3] > boxes[:, 1]))
    if valid.sum() == 0:
        return []
    boxes = boxes[valid]
    confs = confs[valid]
    ids   = ids[valid]

    # 按类别分别做 NMS
    results = []
    for cls_id in np.unique(ids):
        mask_c = ids == cls_id
        keep = _nms(boxes[mask_c], confs[mask_c], NMS_THRESH)
        for k in keep:
            b_arr = boxes[mask_c][k]
            c     = float(confs[mask_c][k])
            cid   = int(cls_id)
            results.append({
                'box':     [float(b_arr[0]), float(b_arr[1]),
                            float(b_arr[2]), float(b_arr[3])],
                'conf':    c,
                'cls':     cid,
                'name':    COCO_CLASSES[cid] if cid < len(COCO_CLASSES) else str(cid),
                'name_zh': COCO_CLASSES_ZH.get(cid, COCO_CLASSES[cid] if cid < len(COCO_CLASSES) else str(cid)),
            })

    # 按置信度排序
    results.sort(key=lambda x: x['conf'], reverse=True)
    return results


class ObjectDetector:
    """
    YOLOv8n-det 目标检测器封装。

    使用示例：
        det = ObjectDetector()
        ok = det.load_model(npu_core=RKNNLite.NPU_CORE_2)
        if ok:
            detections = det.process(rgb_640x640_np, src_w=3840, src_h=2160)
            for d in detections:
                print(d['name_zh'], d['conf'], d['box'])
        det.release()
    """

    def __init__(self, model_path: str = DEFAULT_MODEL_PATH):
        self.model_path = model_path
        self._rknn = None
        self._loaded = False

    def load_model(self, npu_core=None) -> bool:
        """
        加载 RKNN 模型并绑定 NPU Core。
        npu_core: RKNNLite.NPU_CORE_2（默认，携带模式专用）
        失败时优雅降级，返回 False。
        """
        try:
            from rknnlite.api import RKNNLite
            if npu_core is None:
                npu_core = RKNNLite.NPU_CORE_2

            if not os.path.exists(self.model_path):
                import logging
                logging.getLogger(__name__).warning(
                    f'目标检测模型不存在: {self.model_path}，携带模式检测功能不可用'
                )
                return False

            rknn = RKNNLite()
            ret = rknn.load_rknn(self.model_path)
            if ret != 0:
                rknn.release()
                return False
            ret = rknn.init_runtime(core_mask=npu_core)
            if ret != 0:
                rknn.release()
                return False

            self._rknn = rknn
            self._loaded = True
            import logging
            logging.getLogger(__name__).info(
                f'目标检测器 OK（Core {npu_core}），模型: {self.model_path}'
            )
            return True
        except Exception as e:
            import logging
            logging.getLogger(__name__).warning(f'目标检测器加载失败: {e}')
            return False

    def process(self, rgb_np: 'np.ndarray',
                src_w: int, src_h: int,
                content_w: int = 640, content_h: int = 360,
                pad_left: int = 0, pad_top: int = 140,
                obstacle_only: bool = True):
        """
        对单帧进行目标检测。

        参数：
            rgb_np: shape (1, H, W, 3) 或 (H, W, 3)，uint8，RGB格式
                    H=W=640，letterbox 输入
            src_w, src_h: 原始图像分辨率（用于坐标映射回原始空间）
            obstacle_only: 是否只返回障碍物类别

        返回：
            List[dict]，格式同 postprocess_det()
            失败时返回 []
        """
        if not self._loaded or self._rknn is None:
            return []
        try:
            if rgb_np.ndim == 3:
                inp = rgb_np[np.newaxis]  # (1, H, W, 3)
            else:
                inp = rgb_np
            outputs = self._rknn.inference(inputs=[inp])
            return postprocess_det(
                outputs, src_w, src_h,
                dst_w=640, dst_h=640,
                content_w=content_w, content_h=content_h,
                pad_left=pad_left, pad_top=pad_top,
                obstacle_only=obstacle_only,
            )
        except Exception as e:
            import logging
            logging.getLogger(__name__).debug(f'目标检测推理异常: {e}')
            return []

    def release(self):
        """释放 RKNN 资源"""
        if self._rknn is not None:
            try:
                self._rknn.release()
            except Exception:
                pass
            self._rknn = None
        self._loaded = False
