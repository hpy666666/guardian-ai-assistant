"""
postprocess_trunc.py — YOLOv8-pose 截断模型后处理
===================================================
适配截断版 ONNX 转换的 RKNN 模型，该模型输出12个张量：
  每个检测尺度（80x80, 40x40, 20x20）输出4个张量：
    output{i}_box        : (1, 64, H, W)  — DFL 原始分布，未解码
    output{i}_class      : (1, 1,  H, W)  — 置信度 logits，未 sigmoid
    output{i}_kpt        : (1, 34, H*W)   — 17关键点坐标 (x,y交替)
    output{i}_visibility : (1, 17, H*W)   — 17关键点置信度 logits

后处理步骤（全在 CPU 完成）：
  1. sigmoid(class) 得到置信度
  2. DFL softmax 解码 box → (left, top, right, bottom) 距离
  3. 距离 + anchor 坐标 → cx,cy,w,h → x1,y1,x2,y2
  4. letterbox 坐标 → 原图坐标
  5. NMS 去重
"""

import numpy as np

CONF_THRESH = 0.4
NMS_THRESH  = 0.45
DFL_LEN     = 16      # 64 / 4 = 16，固定值


def _sigmoid(x):
    return 1.0 / (1.0 + np.exp(-np.clip(x, -88, 88)))


def _dfl_decode(box_raw, dfl_len=DFL_LEN):
    """
    DFL 解码：将 (4*dfl_len,) 的原始分布解码为 (4,) 的 ltrb 距离
    对应 C++ compute_dfl()
    """
    box_raw = box_raw.reshape(4, dfl_len)          # (4, 16)
    # softmax
    exp_b = np.exp(box_raw - box_raw.max(axis=1, keepdims=True))
    softmax_b = exp_b / exp_b.sum(axis=1, keepdims=True)
    # 加权求和 → 距离
    reg = np.arange(dfl_len, dtype=np.float32)
    ltrb = (softmax_b * reg).sum(axis=1)           # (4,)
    return ltrb


def _process_scale(box_tensor, class_tensor, kpt_tensor, vis_tensor, stride):
    """
    处理单个检测尺度，返回该尺度检测到的候选框列表。

    参数:
      box_tensor   : (64, H*W)  — DFL 原始分布
      class_tensor : (1,  H*W)  — 置信度 logits
      kpt_tensor   : (34, H*W)  — 关键点坐标
      vis_tensor   : (17, H*W)  — 关键点置信度 logits
      stride       : int        — 该尺度的步长（8/16/32）

    返回:
      boxes  : list of [x1,y1,x2,y2]（letterbox 坐标系，单位像素）
      confs  : list of float
      kps    : list of np.array (17,3) [x,y,vis]（letterbox 坐标系）
    """
    grid_len = box_tensor.shape[1]          # H*W
    grid_h = grid_w = int(grid_len ** 0.5)  # 正方形特征图

    conf_map = class_tensor[0]              # (H*W,)  已经是 sigmoid 后的值

    boxes_out = []
    confs_out = []
    kps_out   = []

    for idx in range(grid_len):
        conf = float(conf_map[idx])
        if conf < CONF_THRESH:
            continue

        i = idx // grid_w   # 行
        j = idx  % grid_w   # 列

        # DFL 解码 → ltrb（相对于 anchor 的距离，单位：格子）
        raw = box_tensor[:, idx]          # (64,)
        ltrb = _dfl_decode(raw)           # (4,) = (left, top, right, bottom)

        # anchor 中心坐标
        cx = (j + 0.5) * stride
        cy = (i + 0.5) * stride

        x1 = cx - ltrb[0] * stride
        y1 = cy - ltrb[1] * stride
        x2 = cx + ltrb[2] * stride
        y2 = cy + ltrb[3] * stride

        if x2 <= x1 or y2 <= y1:
            continue

        # 关键点坐标（kpt_tensor 已经是像素坐标，不需要乘 stride）
        kp_xy = kpt_tensor[:, idx].reshape(17, 2)   # (17, 2)
        kp_vis = vis_tensor[:, idx]               # (17,)  已经是 sigmoid 后的值
        kps = np.concatenate([kp_xy, kp_vis[:, None]], axis=1)  # (17, 3)

        boxes_out.append([x1, y1, x2, y2])
        confs_out.append(conf)
        kps_out.append(kps)

    return boxes_out, confs_out, kps_out


def _nms(boxes, scores, iou_thresh):
    """标准 NMS，boxes: (N,4) x1y1x2y2"""
    if len(boxes) == 0:
        return []
    boxes  = np.array(boxes,  dtype=np.float32)
    scores = np.array(scores, dtype=np.float32)
    x1, y1, x2, y2 = boxes[:,0], boxes[:,1], boxes[:,2], boxes[:,3]
    areas  = (x2 - x1) * (y2 - y1)
    order  = scores.argsort()[::-1]
    keep   = []
    while order.size > 0:
        i = order[0]
        keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        w   = np.maximum(0.0, xx2 - xx1)
        h   = np.maximum(0.0, yy2 - yy1)
        iou = w * h / (areas[i] + areas[order[1:]] - w * h + 1e-6)
        order = order[1:][iou < iou_thresh]
    return keep


def postprocess_trunc(outputs, orig_w, orig_h, scale, pad_left, pad_top):
    """
    截断模型后处理主函数。

    参数:
      outputs  : rknn.inference() 返回的列表，共12个张量
      orig_w/h : 原始图像宽高
      scale    : letterbox 缩放比例
      pad_left/pad_top : letterbox 填充量

    返回:
      boxes    : np.array (N,4)    x1y1x2y2，原图坐标
      confs    : np.array (N,)
      kps_list : list of np.array (17,3)，原图坐标
    """
    # outputs 顺序：box0,cls0,kpt0,vis0, box1,cls1,kpt1,vis1, box2,cls2,kpt2,vis2
    strides = [8, 16, 32]

    all_boxes = []
    all_confs = []
    all_kps   = []

    for i, stride in enumerate(strides):
        base = i * 4
        box_t = outputs[base + 0][0]    # (64, H, W) 或 (64, H*W)
        cls_t = outputs[base + 1][0]    # (1,  H, W)
        kpt_t = outputs[base + 2][0]    # (34, H*W)
        vis_t = outputs[base + 3][0]    # (17, H*W)

        # 展平空间维度
        if box_t.ndim == 3:
            C, H, W = box_t.shape
            box_t = box_t.reshape(C, H * W)
        if cls_t.ndim == 3:
            C, H, W = cls_t.shape
            cls_t = cls_t.reshape(C, H * W)

        b, c, k = _process_scale(box_t, cls_t, kpt_t, vis_t, stride)
        all_boxes.extend(b)
        all_confs.extend(c)
        all_kps.extend(k)

    if len(all_boxes) == 0:
        return [], [], []

    # NMS
    keep = _nms(all_boxes, all_confs, NMS_THRESH)
    all_boxes = [all_boxes[i] for i in keep]
    all_confs = [all_confs[i] for i in keep]
    all_kps   = [all_kps[i]   for i in keep]

    # letterbox 坐标 → 原图坐标
    boxes_np = np.array(all_boxes, dtype=np.float32)
    boxes_np[:, 0] = np.clip((boxes_np[:, 0] - pad_left) / scale, 0, orig_w)
    boxes_np[:, 1] = np.clip((boxes_np[:, 1] - pad_top)  / scale, 0, orig_h)
    boxes_np[:, 2] = np.clip((boxes_np[:, 2] - pad_left) / scale, 0, orig_w)
    boxes_np[:, 3] = np.clip((boxes_np[:, 3] - pad_top)  / scale, 0, orig_h)

    kps_list = []
    for kps in all_kps:
        kps = kps.copy()
        kps[:, 0] = np.clip((kps[:, 0] - pad_left) / scale, 0, orig_w)
        kps[:, 1] = np.clip((kps[:, 1] - pad_top)  / scale, 0, orig_h)
        kps_list.append(kps)

    return boxes_np, np.array(all_confs, dtype=np.float32), kps_list
