"""
Guardian Cloud - REST API 服务
功能：提供 HTTP 接口，供前端网页查询 InfluxDB 中的历史数据，以及管理 xiaozhi-server 配置
运行：uvicorn api:app --reload --port 8001

接口列表：
  GET /                                - 服务健康检查
  GET /api/heartrate/{device_id}       - 查询心率+血氧历史（MAX30102，measurement: heartrate）
  GET /api/spo2/{device_id}            - 查询血氧历史（同 heartrate measurement，字段 spo2）
  GET /api/env/{device_id}             - 查询温湿度历史（BME280，measurement: env）
  GET /api/light/{device_id}           - 查询光照强度历史（BH1750，measurement: light）
  GET /api/gas/{device_id}             - 查询气体传感器历史（MQ-4/MQ-7，measurement: gas）
  GET /api/imu/{device_id}             - 查询IMU数据历史
  GET /api/alerts/{device_id}          - 查询告警历史
  GET /api/status/{device_id}          - 查询设备最新状态
  GET /api/devices                     - 查询所有在线设备列表
  GET  /api/tts-voice                  - 读取当前 TTS 音色
  POST /api/tts-voice                  - 修改 TTS 音色
  GET  /api/prompt                     - 读取当前 prompt / 角色配置
  POST /api/prompt                     - 更新 prompt（同步老人姓名到声纹 speakers 和 memory）
  GET  /api/memory                     - 读取 mem_local_short 记忆文件
  DELETE /api/memory                   - 清空记忆文件
  GET  /api/voiceprint/status          - 获取声纹服务状态
  GET  /api/voiceprint/list            - 列出已注册声纹
  POST /api/voiceprint/register        - 注册声纹（multipart audio）
  DELETE /api/voiceprint/delete        - 删除声纹
  POST /api/restart-ai                 - 重启 xiaozhi-server 进程
"""

from fastapi import FastAPI, Query, HTTPException, UploadFile, File, Form
from fastapi.middleware.cors import CORSMiddleware
from influxdb_client import InfluxDBClient
from influxdb_client.domain.write_precision import WritePrecision
from dotenv import load_dotenv
from datetime import datetime
from pydantic import BaseModel
import os
import re
import yaml
import json
import httpx
import asyncio
import subprocess

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
# TTS 配置读写（HuoshanDoubleStreamTTS 参数）
# ──────────────────────────────────────────────

# xiaozhi-server 配置文件路径（相对于此脚本往上两层目录）
_XIAOZHI_CONFIG = os.path.normpath(os.path.join(
    os.path.dirname(__file__),
    "..", "..", "guardian_server", "main", "xiaozhi-server", "data", ".config.yaml"
))

# mem_local_short 记忆文件路径
_MEMORY_FILE = os.path.normpath(os.path.join(
    os.path.dirname(__file__),
    "..", "..", "guardian_server", "main", "xiaozhi-server", "data", ".memory.yaml"
))

# 声纹服务地址
_VP_BASE = "http://127.0.0.1:8002"
_VP_KEY  = "guardian_vp_key"


def _load_cfg() -> dict:
    """加载 xiaozhi-server .config.yaml"""
    if not os.path.exists(_XIAOZHI_CONFIG):
        raise HTTPException(status_code=404, detail=f"配置文件不存在: {_XIAOZHI_CONFIG}")
    with open(_XIAOZHI_CONFIG, "r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def _save_cfg(cfg: dict):
    """写回 xiaozhi-server .config.yaml"""
    with open(_XIAOZHI_CONFIG, "w", encoding="utf-8") as f:
        yaml.dump(cfg, f, allow_unicode=True, default_flow_style=False, sort_keys=False)


# ── 音色读写 ────────────────────────────────────

class TtsVoiceBody(BaseModel):
    speaker: str

@app.get("/api/tts-voice")
def get_tts_voice():
    """读取当前火山 TTS 音色"""
    try:
        cfg = _load_cfg()
        speaker = cfg.get("TTS", {}).get("HuoshanDoubleStreamTTS", {}).get("speaker", "")
        return {"speaker": speaker}
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/tts-voice")
def set_tts_voice(body: TtsVoiceBody):
    """修改火山 TTS 音色，写回配置文件"""
    try:
        cfg = _load_cfg()
        cfg.setdefault("TTS", {}).setdefault("HuoshanDoubleStreamTTS", {})["speaker"] = body.speaker
        _save_cfg(cfg)
        return {"ok": True, "speaker": body.speaker}
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


# ── Prompt / 角色配置 ────────────────────────────

class PromptBody(BaseModel):
    prompt: str
    elder_name: str = ""          # 老人姓名（可选），同步到声纹 speakers 和 memory

@app.get("/api/prompt")
def get_prompt():
    """读取当前 prompt 和老人姓名"""
    try:
        cfg = _load_cfg()
        prompt = cfg.get("prompt", "")
        # 从 voiceprint.speakers[0] 解析老人姓名
        speakers = cfg.get("voiceprint", {}).get("speakers", [])
        elder_name = ""
        if speakers:
            parts = speakers[0].split(",")
            elder_name = parts[1].strip() if len(parts) > 1 else ""
        return {"prompt": prompt, "elder_name": elder_name}
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/prompt")
def set_prompt(body: PromptBody):
    """更新 prompt，并同步老人姓名到 voiceprint.speakers"""
    try:
        cfg = _load_cfg()
        cfg["prompt"] = body.prompt
        # 同步老人姓名到 voiceprint.speakers
        if body.elder_name:
            speakers = cfg.get("voiceprint", {}).get("speakers", [])
            if speakers:
                parts = speakers[0].split(",")
                # 格式: id,姓名,描述
                parts[1] = body.elder_name
                cfg["voiceprint"]["speakers"][0] = ",".join(parts)
            else:
                cfg.setdefault("voiceprint", {})["speakers"] = [
                    f"owner,{body.elder_name},独居老人，本设备主人"
                ]
        _save_cfg(cfg)
        return {"ok": True}
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


# ── 记忆管理 ────────────────────────────────────

@app.get("/api/memory")
def get_memory():
    """读取 mem_local_short 记忆文件（.memory.yaml）"""
    if not os.path.exists(_MEMORY_FILE):
        return {"memory": {}, "exists": False}
    try:
        with open(_MEMORY_FILE, "r", encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
        return {"memory": data, "exists": True}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.delete("/api/memory")
def delete_memory():
    """清空记忆文件"""
    if os.path.exists(_MEMORY_FILE):
        os.remove(_MEMORY_FILE)
    return {"ok": True}


# ── 声纹管理（代理到声纹服务 :8002）────────────────

@app.get("/api/voiceprint/status")
async def get_voiceprint_status():
    """获取声纹服务健康状态"""
    async with httpx.AsyncClient(timeout=3.0) as client:
        try:
            r = await client.get(f"{_VP_BASE}/voiceprint/health", params={"key": _VP_KEY})
            return r.json()
        except Exception as e:
            raise HTTPException(status_code=503, detail=f"声纹服务不可达: {e}")


@app.get("/api/voiceprint/list")
async def list_voiceprints():
    """列出已注册说话人"""
    async with httpx.AsyncClient(timeout=3.0) as client:
        try:
            r = await client.get(f"{_VP_BASE}/voiceprint/list", params={"key": _VP_KEY})
            return r.json()
        except Exception as e:
            raise HTTPException(status_code=503, detail=f"声纹服务不可达: {e}")


@app.post("/api/voiceprint/register")
async def register_voiceprint(
    speaker_id: str = Form(...),
    file: UploadFile = File(...),
    accumulate: bool = Form(False),
):
    """注册声纹：转发音频到声纹服务"""
    audio_bytes = await file.read()
    async with httpx.AsyncClient(timeout=30.0) as client:
        try:
            data = {"speaker_id": speaker_id}
            if accumulate:
                data["accumulate"] = "true"
            r = await client.post(
                f"{_VP_BASE}/voiceprint/register",
                params={"key": _VP_KEY},
                data=data,
                files={"file": (file.filename, audio_bytes, "audio/wav")},
            )
            if r.status_code != 200:
                raise HTTPException(status_code=r.status_code, detail=r.text)
            return r.json()
        except HTTPException:
            raise
        except Exception as e:
            raise HTTPException(status_code=503, detail=f"声纹服务不可达: {e}")


@app.delete("/api/voiceprint/delete")
async def delete_voiceprint(speaker_id: str = Query(...)):
    """删除已注册说话人"""
    async with httpx.AsyncClient(timeout=5.0) as client:
        try:
            r = await client.delete(
                f"{_VP_BASE}/voiceprint/delete",
                params={"key": _VP_KEY, "speaker_id": speaker_id},
            )
            if r.status_code != 200:
                raise HTTPException(status_code=r.status_code, detail=r.text)
            return r.json()
        except HTTPException:
            raise
        except Exception as e:
            raise HTTPException(status_code=503, detail=f"声纹服务不可达: {e}")


# ── 传感器快照（供 xiaozhi-server context_providers 调用）────────────────

def _get_latest(measurement: str, device_id: str, field: str, minutes: int = 10):
    """从 InfluxDB 取最近 N 分钟内某字段的最新一条值，无数据返回 None"""
    flux = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: -{minutes}m)
  |> filter(fn: (r) => r._measurement == "{measurement}")
  |> filter(fn: (r) => r.device_id == "{device_id}")
  |> filter(fn: (r) => r._field == "{field}")
  |> last()
'''
    rows = run_query(flux)
    for row in rows:
        if "error" not in row:
            return row.get("value"), row.get("time")
    return None, None


def _age_label(iso_time: str) -> str:
    """将 ISO 时间戳转为'X分钟前'的友好表示"""
    if not iso_time:
        return "未知"
    try:
        from datetime import timezone
        t = datetime.fromisoformat(iso_time.replace("Z", "+00:00"))
        delta = int((datetime.now(timezone.utc) - t).total_seconds())
        if delta < 60:
            return f"{delta}秒前"
        elif delta < 3600:
            return f"{delta // 60}分钟前"
        else:
            return f"{delta // 3600}小时前"
    except Exception:
        return "未知"


@app.get("/api/sensor-snapshot/{device_id}")
def get_sensor_snapshot(device_id: str):
    """
    供 xiaozhi-server context_providers 调用的传感器快照接口。
    返回格式：{"code": 0, "data": {"心率": "78 bpm（正常）", ...}}
    所有 InfluxDB 查询并发执行，通常 <300ms 完成。
    """
    validate_device_id(device_id)

    from concurrent.futures import ThreadPoolExecutor, as_completed

    # 并发拉取所有字段
    queries = {
        "hr":    ("heartrate",  "heartrate"),
        "spo2":  ("heartrate",  "spo2"),
        "temp":  ("env",        "temperature"),
        "humi":  ("env",        "humidity"),
        "lux":   ("light",      "lux"),
        "mq4":   ("gas",        "mq4_mv"),
        "mq7":   ("gas",        "mq7_mv"),
        "fall":  ("alert_fall", "triggered"),
    }

    results = {}
    with ThreadPoolExecutor(max_workers=8) as pool:
        futures = {
            pool.submit(
                _get_latest,
                meas, device_id, field,
                60 if key == "fall" else 10
            ): key
            for key, (meas, field) in queries.items()
        }
        for future in as_completed(futures):
            key = futures[future]
            try:
                results[key] = future.result()
            except Exception:
                results[key] = (None, None)

    data = {}

    # ── 心率 ──────────────────────────────────────
    hr_val, hr_time = results.get("hr", (None, None))
    if hr_val is not None:
        hr = int(hr_val)
        if hr < 50:
            hr_label = f"{hr} bpm（⚠️ 偏低，请注意）"
        elif hr <= 100:
            hr_label = f"{hr} bpm（正常）"
        elif hr <= 120:
            hr_label = f"{hr} bpm（⚠️ 偏快，注意休息）"
        else:
            hr_label = f"{hr} bpm（⚠️ 过快，请立即关注）"
        data["心率"] = hr_label
    else:
        data["心率"] = "暂无数据"

    # ── 血氧 ──────────────────────────────────────
    spo2_val, _ = results.get("spo2", (None, None))
    if spo2_val is not None:
        spo2 = int(spo2_val)
        if spo2 >= 95:
            spo2_label = f"{spo2}%（正常）"
        elif spo2 >= 90:
            spo2_label = f"{spo2}%（⚠️ 偏低，注意呼吸）"
        else:
            spo2_label = f"{spo2}%（⚠️ 过低，请立即关注）"
        data["血氧"] = spo2_label
    else:
        data["血氧"] = "暂无数据"

    # ── 室温 / 湿度 ───────────────────────────────
    temp_val, _ = results.get("temp", (None, None))
    humi_val, _ = results.get("humi", (None, None))
    env_parts = []
    if temp_val is not None:
        env_parts.append(f"{temp_val:.1f}°C")
    if humi_val is not None:
        env_parts.append(f"湿度 {humi_val:.0f}%")
    data["室内环境"] = "、".join(env_parts) if env_parts else "暂无数据"

    # ── 光照 ──────────────────────────────────────
    lux_val, _ = results.get("lux", (None, None))
    if lux_val is not None:
        lux = int(lux_val)
        if lux < 50:
            lux_label = f"{lux} lux（较暗，注意用眼）"
        elif lux < 500:
            lux_label = f"{lux} lux（正常）"
        else:
            lux_label = f"{lux} lux（较亮）"
        data["光照"] = lux_label
    else:
        data["光照"] = "暂无数据"

    # ── 气体 ──────────────────────────────────────
    mq4_val, _ = results.get("mq4", (None, None))
    mq7_val, _ = results.get("mq7", (None, None))
    gas_alarm = (mq4_val is not None and mq4_val > 2000) or \
                (mq7_val is not None and mq7_val > 2000)
    if mq4_val is not None or mq7_val is not None:
        data["气体安全"] = "⚠️ 检测到异常，请注意通风！" if gas_alarm else "正常"
    else:
        data["气体安全"] = "暂无数据"

    # ── 跌倒 ──────────────────────────────────────
    fall_val, fall_time = results.get("fall", (None, None))
    if fall_val is not None and fall_val:
        data["跌倒状态"] = f"⚠️ {_age_label(fall_time)}检测到疑似跌倒，请确认老人状况！"
    else:
        data["跌倒状态"] = "无异常"

    # ── 数据时效 ──────────────────────────────────
    data["数据更新时间"] = _age_label(hr_time) if hr_time else "设备可能离线"

    return {"code": 0, "data": data}


# ── 重启 AI 服务 ────────────────────────────────

@app.post("/api/restart-ai")
def restart_ai():
    """
    通过 taskkill 结束 xiaozhi-server 的 Python 进程（app.py）。
    start_all.bat 里的进程窗口会自动退出，需要用户手动重启或改造为守护进程。
    当前实现：发送 CTRL_C 信号给监听 8000 端口的进程。
    """
    try:
        # 找到监听 8000 端口的 PID
        result = subprocess.run(
            ["netstat", "-ano"],
            capture_output=True, text=True, timeout=5
        )
        pid = None
        for line in result.stdout.splitlines():
            if ":8000" in line and "LISTENING" in line:
                parts = line.split()
                pid = parts[-1]
                break
        if not pid:
            return {"ok": False, "msg": "未找到监听 8000 端口的进程，服务可能未运行"}
        subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True, timeout=5)
        return {"ok": True, "msg": f"已终止 PID={pid}，请在对应窗口重新运行 app.py"}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
