"""
Guardian Cloud - REST API 服务
功能：提供 HTTP 接口，供前端网页查询 InfluxDB 中的历史数据
运行：uvicorn api:app --reload --port 8000

接口列表：
  GET /                          - 服务健康检查
  GET /api/heartrate/{device_id} - 查询心率+血氧历史（MAX30102，measurement: heartrate）
  GET /api/spo2/{device_id}      - 查询血氧历史（同 heartrate measurement，字段 spo2）
  GET /api/env/{device_id}       - 查询温湿度历史（BME280，measurement: env）
  GET /api/light/{device_id}     - 查询光照强度历史（BH1750，measurement: light）
  GET /api/gas/{device_id}       - 查询气体传感器历史（MQ-4/MQ-7，measurement: gas）
  GET /api/imu/{device_id}       - 查询IMU数据历史
  GET /api/alerts/{device_id}    - 查询告警历史
  GET /api/status/{device_id}    - 查询设备最新状态
  GET /api/devices               - 查询所有在线设备列表
"""

from fastapi import FastAPI, Query, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from influxdb_client import InfluxDBClient
from influxdb_client.domain.write_precision import WritePrecision
from dotenv import load_dotenv
from datetime import datetime
from pydantic import BaseModel
import os
import re
import yaml

load_dotenv()

INFLUX_URL    = os.getenv("INFLUX_URL").rstrip("/")
INFLUX_TOKEN  = os.getenv("INFLUX_TOKEN")
INFLUX_ORG    = os.getenv("INFLUX_ORG")
INFLUX_BUCKET = os.getenv("INFLUX_BUCKET", "guardian")

# ──────────────────────────────────────────────
# FastAPI 应用初始化
# ──────────────────────────────────────────────
app = FastAPI(
    title="Guardian Cloud API",
    description="独居老人安全监护系统 - 云端数据接口",
    version="1.0.0"
)

# 允许跨域（前端网页从不同端口访问时必须开启）
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# 挂载 AI 语音助手路由（/ai/llm, /ai/test 等）
try:
    from ai_server import register_ai_routes
    register_ai_routes(app)
except ImportError:
    pass  # ai_server.py 不存在时忽略

# InfluxDB 查询客户端
influx = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
query_api = influx.query_api()

# ──────────────────────────────────────────────
# 工具函数：参数校验（防 Flux 注入）
# ──────────────────────────────────────────────
_SAFE_ID_RE = re.compile(r'^[a-zA-Z0-9_\-]{1,32}$')

def validate_device_id(device_id: str) -> str:
    """校验 device_id 只包含安全字符，防止 Flux 注入"""
    if not _SAFE_ID_RE.match(device_id):
        raise HTTPException(status_code=400, detail="Invalid device_id format")
    return device_id

def validate_field(field: str) -> str:
    """校验 field 名只包含安全字符"""
    if not _SAFE_ID_RE.match(field):
        raise HTTPException(status_code=400, detail="Invalid field name format")
    return field

# ──────────────────────────────────────────────
# 工具函数：执行 Flux 查询并返回列表
# ──────────────────────────────────────────────
def run_query(flux: str) -> list:
    """执行 Flux 查询，返回 [{time, field, value, device_id}, ...] 列表"""
    try:
        tables = query_api.query(flux, org=INFLUX_ORG)
        results = []
        for table in tables:
            for record in table.records:
                results.append({
                    "time":      record.get_time().isoformat(),
                    "field":     record.get_field(),
                    "value":     record.get_value(),
                    "device_id": record.values.get("device_id", "unknown"),
                })
        return results
    except Exception as e:
        return [{"error": str(e)}]

def build_query(measurement: str, device_id: str, range_minutes: int, field: str = None) -> str:
    """构建标准 Flux 查询语句"""
    flux = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: -{range_minutes}m)
  |> filter(fn: (r) => r._measurement == "{measurement}")
  |> filter(fn: (r) => r.device_id == "{device_id}")
'''
    if field:
        flux += f'  |> filter(fn: (r) => r._field == "{field}")\n'
    flux += '  |> sort(columns: ["_time"], desc: false)'
    return flux

# ──────────────────────────────────────────────
# 接口：健康检查
# ──────────────────────────────────────────────
@app.get("/")
def health_check():
    return {
        "status": "ok",
        "service": "Guardian Cloud API",
        "time": datetime.now().isoformat()
    }

# ──────────────────────────────────────────────
# 接口：心率数据
# ──────────────────────────────────────────────
@app.get("/api/heartrate/{device_id}")
def get_heartrate(
    device_id: str,
    minutes: int = Query(default=60, description="查询最近N分钟的数据")
):
    """
    查询指定设备的心率历史数据
    示例：GET /api/heartrate/001?minutes=30
    """
    validate_device_id(device_id)
    # 不加 field 过滤，返回 heartrate measurement 的所有字段
    # 包含：heartrate（心率）和 spo2（血氧），来自同一个 MAX30102 传感器
    flux = build_query("heartrate", device_id, minutes)
    data = run_query(flux)
    return {
        "device_id": device_id,
        "measurement": "heartrate",
        "range_minutes": minutes,
        "count": len(data),
        "data": data
    }

# ──────────────────────────────────────────────
# 接口：血氧数据
# 注意：spo2 现在和 heartrate 存在同一个 measurement "heartrate" 中
# ──────────────────────────────────────────────
@app.get("/api/spo2/{device_id}")
def get_spo2(
    device_id: str,
    minutes: int = Query(default=60)
):
    """
    查询血氧历史数据
    数据来源：measurement=heartrate，字段 spo2（MAX30102 同一传感器）
    """
    validate_device_id(device_id)
    flux = build_query("heartrate", device_id, minutes, field="spo2")
    data = run_query(flux)
    return {
        "device_id": device_id,
        "measurement": "heartrate",
        "range_minutes": minutes,
        "count": len(data),
        "data": data
    }

# ──────────────────────────────────────────────
# 接口：环境数据（温湿度，BME280）
# ──────────────────────────────────────────────
@app.get("/api/env/{device_id}")
def get_env(
    device_id: str,
    minutes: int = Query(default=60),
    field: str = Query(default=None, description="temperature / humidity，不填返回全部")
):
    """查询 BME280 温湿度历史"""
    validate_device_id(device_id)
    if field:
        validate_field(field)
    flux = build_query("env", device_id, minutes, field=field)
    data = run_query(flux)
    return {
        "device_id": device_id,
        "measurement": "env",
        "range_minutes": minutes,
        "count": len(data),
        "data": data
    }

# ──────────────────────────────────────────────
# 接口：光照强度（BH1750）
# ──────────────────────────────────────────────
@app.get("/api/light/{device_id}")
def get_light(
    device_id: str,
    minutes: int = Query(default=60)
):
    """查询 BH1750 光照强度历史，字段 lux"""
    validate_device_id(device_id)
    flux = build_query("light", device_id, minutes, field="lux")
    data = run_query(flux)
    return {
        "device_id": device_id,
        "measurement": "light",
        "range_minutes": minutes,
        "count": len(data),
        "data": data
    }

# ──────────────────────────────────────────────
# 接口：气体传感器（MQ-4 甲烷 / MQ-7 CO）
# ──────────────────────────────────────────────
@app.get("/api/gas/{device_id}")
def get_gas(
    device_id: str,
    minutes: int = Query(default=60),
    field: str = Query(default=None, description="mq4_mv / mq7_mv，不填返回全部")
):
    """查询 MQ-4/MQ-7 气体传感器历史（单位：mV）"""
    validate_device_id(device_id)
    if field:
        validate_field(field)
    flux = build_query("gas", device_id, minutes, field=field)
    data = run_query(flux)
    return {
        "device_id": device_id,
        "measurement": "gas",
        "range_minutes": minutes,
        "count": len(data),
        "data": data
    }

# ──────────────────────────────────────────────
# 接口：GPS 位置
# ──────────────────────────────────────────────
@app.get("/api/location/{device_id}")
def get_location(
    device_id: str,
    minutes: int = Query(default=5, description="查询最近N分钟的位置数据")
):
    """查询 GPS 经纬度历史，前端用于地图轨迹展示"""
    validate_device_id(device_id)
    flux = build_query("location", device_id, minutes)
    data = run_query(flux)
    return {
        "device_id": device_id,
        "measurement": "location",
        "range_minutes": minutes,
        "count": len(data),
        "data": data
    }

# ──────────────────────────────────────────────
# 接口：IMU 数据（Roll/Pitch/加速度/跌倒状态）
# ──────────────────────────────────────────────
@app.get("/api/imu/{device_id}")
def get_imu(
    device_id: str,
    minutes: int = Query(default=30, description="查询最近N分钟的IMU数据")
):
    """查询 MPU6050 姿态数据"""
    validate_device_id(device_id)
    flux = build_query("imu", device_id, minutes)
    data = run_query(flux)
    return {
        "device_id": device_id,
        "measurement": "imu",
        "range_minutes": minutes,
        "count": len(data),
        "data": data
    }

# ──────────────────────────────────────────────
# 接口：告警历史
# ──────────────────────────────────────────────
@app.get("/api/alerts/{device_id}")
def get_alerts(
    device_id: str,
    minutes: int = Query(default=1440, description="默认查最近24小时")
):
    """查询跌倒和气体告警历史"""
    validate_device_id(device_id)
    results = []
    for alert_type in ["alert_fall", "alert_gas"]:
        flux = build_query(alert_type, device_id, minutes)
        data = run_query(flux)
        for item in data:
            item["alert_type"] = alert_type.replace("alert_", "")
            results.append(item)
    # 按时间排序
    results.sort(key=lambda x: x.get("time", ""), reverse=True)
    return {
        "device_id": device_id,
        "range_minutes": minutes,
        "count": len(results),
        "data": results
    }

# ──────────────────────────────────────────────
# 接口：设备最新状态
# ──────────────────────────────────────────────
@app.get("/api/status/{device_id}")
def get_status(device_id: str):
    """查询设备最新一条状态数据（在线/离线、电量、SD卡、4G）"""
    validate_device_id(device_id)
    flux = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: -10m)
  |> filter(fn: (r) => r._measurement == "status")
  |> filter(fn: (r) => r.device_id == "{device_id}")
  |> last()
'''
    data = run_query(flux)
    # 把多个字段合并成一条记录
    merged = {"device_id": device_id, "time": None, "online": False}
    for item in data:
        field = item.get("field")
        value = item.get("value")
        if field == "status":
            merged["status"]  = value
            merged["time"]    = item.get("time")
            merged["online"]  = True
        elif field == "battery":
            merged["battery"] = value
        elif field == "sd":
            merged["sd"] = int(value) if value is not None else 0
        elif field == "lte":
            merged["lte"] = int(value) if value is not None else 0
    return merged

# ──────────────────────────────────────────────
# 接口：所有设备列表（最近5分钟有上报的）
# ──────────────────────────────────────────────
@app.get("/api/devices")
def get_devices():
    """返回最近5分钟内有数据上报的设备ID列表"""
    flux = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: -5m)
  |> keep(columns: ["device_id"])
  |> distinct(column: "device_id")
'''
    try:
        tables = query_api.query(flux, org=INFLUX_ORG)
        devices = []
        for table in tables:
            for record in table.records:
                val = record.get_value()
                if val and val not in devices:
                    devices.append(val)
        return {"count": len(devices), "devices": devices}
    except Exception as e:
        return {"error": str(e)}


# ──────────────────────────────────────────────
# TTS 配置读写（PaddleSpeechTTS 参数）
# ──────────────────────────────────────────────

# xiaozhi-server 配置文件路径（相对于此脚本往上两层目录）
_XIAOZHI_CONFIG = os.path.join(
    os.path.dirname(__file__),
    "..", "..", "xiaozhi-esp32-server", "main", "xiaozhi-server", "data", ".config.yaml"
)

class TtsConfig(BaseModel):
    spk_id: int = 0
    speed: float = 1.0
    volume: float = 1.0

@app.get("/api/tts-config")
def get_tts_config():
    """读取 PaddleSpeechTTS 当前参数"""
    cfg_path = os.path.normpath(_XIAOZHI_CONFIG)
    if not os.path.exists(cfg_path):
        raise HTTPException(status_code=404, detail=f"配置文件不存在: {cfg_path}")
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            cfg = yaml.safe_load(f)
        tts_cfg = cfg.get("TTS", {}).get("PaddleSpeechTTS", {})
        return {
            "spk_id": tts_cfg.get("spk_id", 0),
            "speed":  tts_cfg.get("speed",  1.0),
            "volume": tts_cfg.get("volume", 1.0),
        }
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.post("/api/tts-config")
def set_tts_config(body: TtsConfig):
    """更新 PaddleSpeechTTS 参数并写回 .config.yaml"""
    cfg_path = os.path.normpath(_XIAOZHI_CONFIG)
    if not os.path.exists(cfg_path):
        raise HTTPException(status_code=404, detail=f"配置文件不存在: {cfg_path}")
    # 限制范围
    if not (0 <= body.spk_id <= 10):
        raise HTTPException(status_code=400, detail="spk_id 必须在 0~10 之间")
    if not (0.5 <= body.speed <= 2.0):
        raise HTTPException(status_code=400, detail="speed 必须在 0.5~2.0 之间")
    if not (0.5 <= body.volume <= 2.0):
        raise HTTPException(status_code=400, detail="volume 必须在 0.5~2.0 之间")
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            cfg = yaml.safe_load(f)
        cfg.setdefault("TTS", {}).setdefault("PaddleSpeechTTS", {})
        cfg["TTS"]["PaddleSpeechTTS"]["spk_id"] = body.spk_id
        cfg["TTS"]["PaddleSpeechTTS"]["speed"]  = body.speed
        cfg["TTS"]["PaddleSpeechTTS"]["volume"] = body.volume
        with open(cfg_path, "w", encoding="utf-8") as f:
            yaml.dump(cfg, f, allow_unicode=True, default_flow_style=False, sort_keys=False)
        return {"ok": True, "spk_id": body.spk_id, "speed": body.speed, "volume": body.volume}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
