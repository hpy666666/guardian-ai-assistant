"""
pose_analyzer.py — Guardian 姿态分析与摔倒检测模块 v2
======================================================

改进点（相比 v1）：
1. 新增速度特征：躯干中心 Y 坐标下降速度，检测摔倒"过程"
2. 放宽单指标触发：强指标（躯干角>70° 或 宽高比>1.5）单独即可进入可疑
3. 减少确认帧数：2帧可疑 + 3帧确认（原来 3+5），更快响应
4. 静止躺地检测：进入 FALLEN 后若速度为0但姿态仍异常，持续告警
5. 关键点缺失降级：踝关节不可见时只用宽高比+躯干角，不依赖头部高度比

关键点索引（COCO 17-kp）
------------------------
0:鼻 1:左眼 2:右眼 3:左耳 4:右耳
5:左肩 6:右肩 7:左肘 8:右肘 9:左腕 10:右腕
11:左髋 12:右髋 13:左膝 14:右膝 15:左踝 16:右踝
"""

import time
import math
import numpy as np
from dataclasses import dataclass, field
from typing import List, Optional, Tuple
from collections import deque

# ── 检测参数 ──────────────────────────────────────────────────────────
# 静态姿态阈值
BBOX_RATIO_THRESH      = 1.2   # 宽/高 > 此值：可疑（横躺）
BBOX_RATIO_STRONG      = 1.5   # 宽/高 > 此值：强指标，单独触发
TRUNK_ANGLE_THRESH     = 55.0  # 躯干角(°) > 此值：可疑
TRUNK_ANGLE_STRONG     = 70.0  # 躯干角(°) > 此值：强指标，单独触发
HEAD_HEIGHT_THRESH     = 0.35  # 头部高度比 < 此值：可疑（需踝关节可见）
KP_CONF_MIN            = 0.3   # 关键点置信度阈值

# 速度特征阈值
VELOCITY_WINDOW        = 6     # 计算速度用的历史帧数（约0.3s@20fps）
FALL_VELOCITY_THRESH   = 15.0  # 躯干中心 Y 下降速度(px/帧) > 此值：触发速度指标
                                # 3840x2160 原图空间，15px/帧 ≈ 0.9m/s

# 状态机参数
SUSPICIOUS_FRAMES      = 2     # 进入 SUSPICIOUS 需连续 N 帧满足触发条件
FALLEN_CONFIRM_FRAMES  = 3     # SUSPICIOUS→FALLEN 需连续 N 帧满足触发条件
RECOVER_FRAMES         = 20    # FALLEN→NORMAL 需连续 N 帧正常（约1s@20fps）
ALERT_INTERVAL_SEC     = 5.0   # FALLEN 状态下告警最短间隔（秒）


# ── 数据结构 ──────────────────────────────────────────────────────────

class FallState:
    NORMAL     = 'NORMAL'
    SUSPICIOUS = 'SUSPICIOUS'
    FALLEN     = 'FALLEN'


@dataclass
class FallFeatures:
    """单帧特征"""
    bbox_ratio:        float = 0.0
    trunk_angle:       float = 0.0
    head_height_ratio: float = 1.0
    fall_velocity:     float = 0.0   # 躯干中心 Y 下降速度（px/帧）
    # 各指标触发
    bbox_suspicious:   bool  = False
    trunk_suspicious:  bool  = False
    head_suspicious:   bool  = False
    velocity_suspicious: bool = False
    valid_kp_count:    int   = 0

    @property
    def trigger_count(self) -> int:
        return (int(self.bbox_suspicious) + int(self.trunk_suspicious)
                + int(self.head_suspicious) + int(self.velocity_suspicious))

    @property
    def strong_trigger(self) -> bool:
        """任一强指标单独触发"""
        return (self.bbox_ratio > BBOX_RATIO_STRONG
                or self.trunk_angle > TRUNK_ANGLE_STRONG
                or self.velocity_suspicious)


@dataclass
class PersonState:
    """单人完整状态（跨帧持久化）"""
    track_id:          int   = -1
    state:             str   = FallState.NORMAL
    suspicious_count:  int   = 0
    fallen_count:      int   = 0
    recover_count:     int   = 0
    last_alert_time:   float = 0.0
    features:          Optional[FallFeatures] = None
    alert_triggered:   bool  = False
    box:               Optional[np.ndarray] = None
    conf:              float = 0.0


# ── 辅助函数 ──────────────────────────────────────────────────────────

def _kp(kps: np.ndarray, idx: int) -> Tuple[float, float, float]:
    return float(kps[idx, 0]), float(kps[idx, 1]), float(kps[idx, 2])

def _midpoint(a, b) -> Tuple[float, float]:
    return (a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0

def _valid(conf: float) -> bool:
    return conf >= KP_CONF_MIN


def extract_features(box: np.ndarray, kps: np.ndarray,
                     center_history: deque) -> FallFeatures:
    """
    从 bbox + 17关键点 + 历史中心点队列 提取摔倒特征。
    center_history: deque，存储近 VELOCITY_WINDOW 帧的躯干中心 (cx, cy)
    """
    feat = FallFeatures()
    x1, y1, x2, y2 = box
    bw = max(float(x2 - x1), 1.0)
    bh = max(float(y2 - y1), 1.0)

    # ── 1. 边界框宽高比 ──
    feat.bbox_ratio = bw / bh
    feat.bbox_suspicious = feat.bbox_ratio > BBOX_RATIO_THRESH

    # ── 2. 躯干倾斜角 ──
    ls_x, ls_y, ls_c = _kp(kps, 5)
    rs_x, rs_y, rs_c = _kp(kps, 6)
    lh_x, lh_y, lh_c = _kp(kps, 11)
    rh_x, rh_y, rh_c = _kp(kps, 12)

    shoulder_valid = _valid(ls_c) and _valid(rs_c)
    hip_valid      = _valid(lh_c) and _valid(rh_c)

    torso_cx, torso_cy = (x1 + x2) / 2, (y1 + y2) / 2  # 默认用 bbox 中心

    if shoulder_valid and hip_valid:
        mx_s, my_s = _midpoint((ls_x, ls_y), (rs_x, rs_y))
        mx_h, my_h = _midpoint((lh_x, lh_y), (rh_x, rh_y))
        dx = mx_h - mx_s
        dy = my_h - my_s
        angle = math.degrees(math.atan2(abs(dx), abs(dy) + 1e-6))
        feat.trunk_angle = angle
        feat.trunk_suspicious = angle > TRUNK_ANGLE_THRESH
        feat.valid_kp_count += 4
        torso_cx = (mx_s + mx_h) / 2
        torso_cy = (my_s + my_h) / 2
    elif shoulder_valid:
        dx = rs_x - ls_x
        dy = rs_y - ls_y
        angle = abs(math.degrees(math.atan2(dy, dx + 1e-6)))
        if angle > 90:
            angle = 180 - angle
        feat.trunk_angle = 90 - angle
        feat.trunk_suspicious = feat.trunk_angle > TRUNK_ANGLE_THRESH
        feat.valid_kp_count += 2
        torso_cx = (ls_x + rs_x) / 2
        torso_cy = (ls_y + rs_y) / 2

    # ── 3. 头部相对高度比（需踝关节可见）──
    nose_x, nose_y, nose_c = _kp(kps, 0)
    la_x, la_y, la_c = _kp(kps, 15)
    ra_x, ra_y, ra_c = _kp(kps, 16)
    head_valid  = _valid(nose_c)
    ankle_valid = _valid(la_c) or _valid(ra_c)

    if head_valid and ankle_valid:
        ankle_y = max(
            la_y if _valid(la_c) else 0.0,
            ra_y if _valid(ra_c) else 0.0
        )
        body_height = abs(ankle_y - nose_y)
        if body_height > 20:
            feat.head_height_ratio = (ankle_y - nose_y) / body_height
            feat.head_suspicious = feat.head_height_ratio < HEAD_HEIGHT_THRESH
            feat.valid_kp_count += 2

    # ── 4. 躯干中心 Y 下降速度 ──
    center_history.append((torso_cx, torso_cy))
    if len(center_history) >= 3:
        # 取最近 N 帧的线性回归斜率（dy/帧），比直接差分更鲁棒
        ys = np.array([c[1] for c in center_history], dtype=np.float32)
        n  = len(ys)
        xs = np.arange(n, dtype=np.float32)
        # 最小二乘斜率
        slope = (n * (xs * ys).sum() - xs.sum() * ys.sum()) / \
                (n * (xs**2).sum() - xs.sum()**2 + 1e-6)
        feat.fall_velocity = float(slope)   # px/帧，正值=向下移动
        feat.velocity_suspicious = feat.fall_velocity > FALL_VELOCITY_THRESH

    return feat


# ── 状态机 ──────────────────────────────────────────────────────────

class PersonTracker:
    def __init__(self, track_id: int):
        self.track_id = track_id
        self.state = PersonState(track_id=track_id)
        self._last_seen = time.time()
        # 躯干中心历史（用于速度计算）
        self._center_history: deque = deque(maxlen=VELOCITY_WINDOW)

    @property
    def is_stale(self) -> bool:
        return time.time() - self._last_seen > 2.0

    def update(self, box: np.ndarray, kps: np.ndarray, conf: float) -> PersonState:
        self._last_seen = time.time()
        now = time.time()

        feat = extract_features(box, kps, self._center_history)

        # 触发条件：强指标单独触发 OR 普通指标 ≥2 个
        fall_likely = feat.strong_trigger or feat.trigger_count >= 2

        ps = self.state
        ps.box     = box.copy()
        ps.conf    = conf
        ps.features = feat
        ps.alert_triggered = False

        if ps.state == FallState.NORMAL:
            if fall_likely:
                ps.suspicious_count += 1
                ps.recover_count = 0
                if ps.suspicious_count >= SUSPICIOUS_FRAMES:
                    ps.state = FallState.SUSPICIOUS
                    ps.fallen_count = 0
            else:
                ps.suspicious_count = max(0, ps.suspicious_count - 1)

        elif ps.state == FallState.SUSPICIOUS:
            if fall_likely:
                ps.fallen_count += 1
                ps.recover_count = 0
                if ps.fallen_count >= FALLEN_CONFIRM_FRAMES:
                    ps.state = FallState.FALLEN
                    if now - ps.last_alert_time >= ALERT_INTERVAL_SEC:
                        ps.alert_triggered = True
                        ps.last_alert_time = now
            else:
                ps.suspicious_count = max(0, ps.suspicious_count - 1)
                ps.fallen_count = 0
                if ps.suspicious_count == 0:
                    ps.state = FallState.NORMAL

        elif ps.state == FallState.FALLEN:
            if fall_likely:
                ps.recover_count = 0
                if now - ps.last_alert_time >= ALERT_INTERVAL_SEC:
                    ps.alert_triggered = True
                    ps.last_alert_time = now
            else:
                ps.recover_count += 1
                if ps.recover_count >= RECOVER_FRAMES:
                    ps.state = FallState.NORMAL
                    ps.suspicious_count = 0
                    ps.fallen_count = 0
                    ps.recover_count = 0

        return ps


# ── 多人管理器 ────────────────────────────────────────────────────────

class FallDetector:
    def __init__(self):
        self._trackers: List[PersonTracker] = []
        self._next_id = 0

    def update(self, boxes, confs, kps_list) -> List[PersonState]:
        self._trackers = [t for t in self._trackers if not t.is_stale]

        if len(boxes) == 0:
            return []

        if not isinstance(boxes, np.ndarray):
            boxes = np.array(boxes)
        if not isinstance(confs, np.ndarray):
            confs = np.array(confs)

        results = []
        matched_tracker_ids = set()

        for box, conf, kps in zip(boxes, confs, kps_list):
            best_tracker = None
            best_iou = 0.4

            for t in self._trackers:
                if id(t) in matched_tracker_ids:
                    continue
                if t.state.box is None:
                    continue
                iou = _iou(box, t.state.box)
                if iou > best_iou:
                    best_iou = iou
                    best_tracker = t

            if best_tracker is None:
                best_tracker = PersonTracker(self._next_id)
                self._next_id += 1
                self._trackers.append(best_tracker)

            matched_tracker_ids.add(id(best_tracker))
            ps = best_tracker.update(box, kps, float(conf))
            results.append(ps)

        return results

    @property
    def alert_count(self) -> int:
        return sum(1 for t in self._trackers
                   if t.state.state == FallState.FALLEN)


def _iou(boxA: np.ndarray, boxB: np.ndarray) -> float:
    xA = max(boxA[0], boxB[0]); yA = max(boxA[1], boxB[1])
    xB = min(boxA[2], boxB[2]); yB = min(boxA[3], boxB[3])
    inter = max(0, xB - xA) * max(0, yB - yA)
    if inter == 0:
        return 0.0
    aA = (boxA[2]-boxA[0]) * (boxA[3]-boxA[1])
    aB = (boxB[2]-boxB[0]) * (boxB[3]-boxB[1])
    return inter / (aA + aB - inter + 1e-6)
