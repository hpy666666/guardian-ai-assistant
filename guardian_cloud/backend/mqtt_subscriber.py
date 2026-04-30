"""
Guardian Cloud - MQTT 订阅 + InfluxDB 存储 + 微信告警模块
功能：
  1. 连接 EMQX Cloud，订阅所有设备 Topic
  2. 收到消息后解析并写入 InfluxDB Cloud
  3. 告警消息通过 Server酱 推送微信通知
"""

import json
import ssl
import time
import requests
from datetime import datetime
import paho.mqtt.client as mqtt
from dotenv import load_dotenv
import os
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
from influxdb_client.domain.write_precision import WritePrecision

# ──────────────────────────────────────────────
# 加载配置
# ──────────────────────────────────────────────
load_dotenv()

BROKER   = os.getenv("MQTT_BROKER")
PORT     = int(os.getenv("MQTT_PORT", 1883))
USERNAME = os.getenv("MQTT_USERNAME")
PASSWORD = os.getenv("MQTT_PASSWORD")
USE_TLS  = os.getenv("MQTT_TLS", "false").lower() == "true"

INFLUX_URL    = os.getenv("INFLUX_URL").rstrip("/")
INFLUX_TOKEN  = os.getenv("INFLUX_TOKEN")
INFLUX_ORG    = os.getenv("INFLUX_ORG")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "guardian")

SERVERCHAN_KEY = os.getenv("SERVERCHAN_KEY", "")

# ──────────────────────────────────────────────
# 告警推送开关（调试阶段设为 False，正式部署改为 True）
# 控制所有 Server酱 微信推送，不影响 InfluxDB 写入
# ──────────────────────────────────────────────
ALERT_ENABLED = False

# 订阅的所有 Topic
# 话题结构（与 ESP32 main.c 保持同步）：
#   heartrate  → MAX30102：心率 + 血氧
#   env        → BME280：温度 + 湿度
#   light      → BH1750：光照强度
#   gas        → MQ-4 + MQ-7：气体传感器电压
#   location   → GPS：经纬度
#   imu        → MPU6050：姿态角 + 加速度 + 跌倒状态
#   status     → 设备状态：在线/SD/LTE/电量
#   alert/fall → 跌倒告警（QoS1）
#   alert/gas  → 气体告警（QoS1）
#   vision     → 泰山派视觉：人脸识别/跌倒 (event: status|fall)
TOPICS = [
    "guardian/+/heartrate",
    "guardian/+/env",
    "guardian/+/light",
    "guardian/+/gas",
    "guardian/+/location",
    "guardian/+/imu",
    "guardian/+/status",
    "guardian/+/alert/fall",
    "guardian/+/alert/gas",
    "guardian/+/vision",        # 泰山派：视觉结果（人脸识别/跌倒检测）
    "guardian/+/antiscam",      # 泰山派：反诈骗告警
]

# ──────────────────────────────────────────────
# InfluxDB 客户端初始化
# ──────────────────────────────────────────────
influx_client = InfluxDBClient(
    url=INFLUX_URL,
    token=INFLUX_TOKEN,
    org=INFLUX_ORG
)
write_api = influx_client.write_api(write_options=SYNCHRONOUS)

def now():
    return datetime.now().strftime("%H:%M:%S")

# ──────────────────────────────────────────────
# 微信推送（Server酱）
# ──────────────────────────────────────────────
# 告警冷却：同一设备同类型告警，60秒内只推送一次，防止刷屏
_alert_cooldown: dict = {}   # key: "device_id:alert_type" → 上次推送时间戳

def send_wechat_alert(device_id: str, alert_type: str, data: dict):
    """通过 Server酱 推送微信告警"""
    if not ALERT_ENABLED:
        print(f"  [微信] 告警推送已关闭（ALERT_ENABLED=False），跳过: {alert_type}")
        return
    if not SERVERCHAN_KEY:
        print("  [微信] SERVERCHAN_KEY 未配置，跳过推送")
        return

    # 冷却检查（60秒内同类告警不重复推）
    cooldown_key = f"{device_id}:{alert_type}"
    now_ts = time.time()
    if cooldown_key in _alert_cooldown:
        elapsed = now_ts - _alert_cooldown[cooldown_key]
        if elapsed < 60:
            print(f"  [微信] 冷却中（{60 - int(elapsed)}秒后可再推），跳过")
            return
    _alert_cooldown[cooldown_key] = now_ts

    # 构建推送内容
    alert_names = {
        "fall": "跌倒告警",
        "gas":  "气体告警",
    }
    alert_name = alert_names.get(alert_type, f"{alert_type}告警")
    timestamp  = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    title   = f"⚠️ [{alert_name}] 设备 {device_id}"
    content = (
        f"**时间**：{timestamp}  \n"
        f"**设备**：{device_id}  \n"
        f"**类型**：{alert_name}  \n"
        f"**数据**：{json.dumps(data, ensure_ascii=False)}  \n"
    )

    try:
        resp = requests.post(
            f"https://sctapi.ftqq.com/{SERVERCHAN_KEY}.send",
            data={"title": title, "desp": content},
            timeout=5
        )
        result = resp.json()
        if result.get("code") == 0:
            print(f"  [微信] 推送成功: {title}")
        else:
            print(f"  [微信] 推送失败: {result}")
    except Exception as e:
        print(f"  [微信] 推送异常: {e}")

# ──────────────────────────────────────────────
# 数据写入 InfluxDB
# ──────────────────────────────────────────────
def write_to_influx(device_id: str, data_type: str, data: dict):
    """
    将消息写入 InfluxDB
    data_type 示例: heartrate / spo2 / env / alert/fall
    """
    try:
        # measurement 用 data_type，把 / 换成 _ 避免歧义
        # 例如 alert/fall → alert_fall
        measurement = data_type.replace("/", "_")

        point = Point(measurement).tag("device_id", device_id)

        # 把消息里所有数值字段都写进去
        has_field = False
        for key, value in data.items():
            if isinstance(value, (int, float)):
                point = point.field(key, float(value))
                has_field = True
            elif isinstance(value, str):
                point = point.field(key, value)
                has_field = True

        if not has_field:
            print(f"  [InfluxDB] 跳过：没有可写入的字段")
            return

        write_api.write(
            bucket=INFLUX_BUCKET,
            org=INFLUX_ORG,
            record=point,
            write_precision=WritePrecision.S
        )
        print(f"  [InfluxDB] 写入成功: {measurement} | device={device_id} | {data}")

    except Exception as e:
        print(f"  [InfluxDB] 写入失败: {e}")

# ──────────────────────────────────────────────
# MQTT 回调：连接成功
# ──────────────────────────────────────────────
def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"[{now()}] 已连接到 EMQX Cloud: {BROKER}")
        for topic in TOPICS:
            # 告警 topic 使用 QoS 1 保证至少一次投递
            qos = 1 if "alert" in topic else 0
            client.subscribe(topic, qos=qos)
            print(f"  -> 订阅: {topic} (QoS {qos})")
        print()
    else:
        print(f"[{now()}] 连接失败，错误码: {reason_code}")

# ──────────────────────────────────────────────
# MQTT 回调：收到消息
# ──────────────────────────────────────────────
def on_message(client, userdata, msg):
    topic       = msg.topic
    payload_str = msg.payload.decode("utf-8")

    # 解析 JSON
    try:
        data = json.loads(payload_str)
    except json.JSONDecodeError:
        print(f"  [警告] 收到非JSON消息: {payload_str}")
        return

    # 从 Topic 提取设备ID和数据类型
    parts     = topic.split("/")
    device_id = parts[1] if len(parts) >= 2 else "unknown"
    data_type = "/".join(parts[2:])

    # 打印收到的消息
    print(f"[{now()}] 收到消息  设备={device_id}  类型={data_type}")
    print(f"  数据: {json.dumps(data, ensure_ascii=False)}")

    # vision 消息特殊处理（泰山派人脸识别/跌倒检测）
    if data_type == "vision":
        event = data.get("event", "")
        if event == "fall":
            identity = data.get("identity", "unknown")
            score    = data.get("score", -1)
            print(f"  📷 [视觉-跌倒] 设备 {device_id} 检测到跌倒！身份={identity} 置信度={score}")
            send_wechat_alert(device_id, "vision_fall", data)
        elif event == "status":
            persons  = data.get("persons", 0)
            known    = data.get("known", [])
            stranger = data.get("stranger_count", 0)
            fps      = data.get("fps", 0)
            print(f"  📷 [视觉-状态] 设备 {device_id} | 人数={persons} 已知={known} 陌生={stranger} FPS={fps}")
        else:
            print(f"  📷 [视觉] 设备 {device_id} event={event}")

    # antiscam 反诈骗告警
    elif data_type == "antiscam":
        event = data.get("event", "")
        if event == "scam_alert":
            risk_score = data.get("risk_score", 0)
            hit_types  = data.get("hit_types", [])
            text       = data.get("text", "")
            print(f"  🚨 [反诈骗告警] 设备 {device_id} | 风险分={risk_score} 命中={hit_types}")
            print(f"     对话内容: {text}")
            send_wechat_alert(device_id, "scam_alert", data)
        else:
            print(f"  🔍 [反诈骗] 设备 {device_id} event={event}")

    # 告警特殊处理：推送微信
    elif "alert" in topic:
        alert_type = parts[-1]  # fall / gas
        print(f"  ⚠️  [告警] 设备 {device_id} 触发 [{alert_type}] 告警！")
        send_wechat_alert(device_id, alert_type, data)

    # 写入 InfluxDB（vision/status 频率高，只写 fall 事件）
    if data_type == "vision":
        if data.get("event") == "fall":
            write_to_influx(device_id, "vision_fall", data)
        # status 每2秒一条，也写入，方便 Grafana 看实时人数/FPS
        elif data.get("event") == "status":
            write_to_influx(device_id, "vision_status", {
                "persons":        data.get("persons", 0),
                "stranger_count": data.get("stranger_count", 0),
                "fps":            data.get("fps", 0),
            })
    elif data_type == "antiscam":
        if data.get("event") == "scam_alert":
            write_to_influx(device_id, "antiscam_alert", {
                "risk_score": data.get("risk_score", 0),
                "text":       data.get("text", ""),
            })
    else:
        write_to_influx(device_id, data_type, data)

# ──────────────────────────────────────────────
# MQTT 回调：断开连接
# ──────────────────────────────────────────────
def on_disconnect(client, userdata, flags, reason_code, properties):
    print(f"[{now()}] 连接已断开，原因码: {reason_code}")
    if reason_code != 0:
        print("  正在尝试重连...")

# ──────────────────────────────────────────────
# 主程序
# ──────────────────────────────────────────────
def main():
    print("=" * 55)
    print("  Guardian Cloud - MQTT 订阅 + InfluxDB 存储服务")
    print("=" * 55)
    print(f"  MQTT Broker : {BROKER}:{PORT}")
    print(f"  InfluxDB    : {INFLUX_URL}")
    print(f"  Bucket      : {INFLUX_BUCKET}")
    print("=" * 55)

    # 验证 InfluxDB 连接（Cloud 免费版 /health 返回 fail 属正常，实际写入不受影响）
    try:
        health = influx_client.health()
        status = health.status
        if status == "fail":
            print(f"  InfluxDB 状态: {status}（Cloud 免费版正常现象，不影响写入）\n")
        else:
            print(f"  InfluxDB 状态: {status}\n")
    except Exception as e:
        print(f"  [错误] InfluxDB 连接失败: {e}\n")

    # 创建 MQTT 客户端
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"guardian-backend-{int(time.time())}",
        protocol=mqtt.MQTTv5
    )
    client.username_pw_set(USERNAME, PASSWORD)

    if USE_TLS:
        client.tls_set(cert_reqs=ssl.CERT_NONE)
        client.tls_insecure_set(True)
        print("  TLS: 已启用\n")

    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect

    print(f"正在连接到 {BROKER}:{PORT} ...")
    client.connect(BROKER, PORT, keepalive=60)

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print(f"\n[{now()}] 手动停止")
        client.disconnect()
        influx_client.close()

if __name__ == "__main__":
    main()
