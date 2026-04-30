# Guardian AI Assistant

> 面向独居老人的智能安全监护终端
> 全国大学生嵌入式芯片与系统设计竞赛 2026 · 睿赛德赛道 · RT-Thread Smart 方向

---

## 项目概述

**Guardian** 是一套穿戴式智能监护系统，专为独居老人设计，具备健康监测、AI 语音交互、异常预警与云端数据看板等功能。核心架构基于三芯片协同：

| 芯片 | 系统 | 职责 |
|------|------|------|
| STM32F407 | RT-Thread 标准版 | 多传感器采集、跌倒检测算法、本地报警 |
| ESP32-S3 | ESP-IDF + FreeRTOS | AI 语音助手、MQTT 云端通信、传感器数据转发 |
| K230（后期） | RT-Thread Smart | 视觉 AI 推理、多进程隔离架构（竞赛核心） |

---

## 核心创新点

1. **RT-Smart 多进程隔离**：AI 推理进程崩溃不影响实时报警进程，端到端延迟 ≤ 200ms
2. **跌倒双验证**：MPU6050 加速度冲击检测 + 摄像头 YOLO-Pose 姿态识别，融合降低误报
3. **AI 语音助手**：基于 xiaozhi-esp32-server，集成 FunASR（本地）+ 阿里云 LLM + 火山引擎 TTS，全程低延迟
4. **声纹识别**：resemblyzer 256 维声纹嵌入，余弦相似度比对，自动识别家庭成员与陌生人
5. **AI 实时感知传感器**：每次对话前自动刷新传感器快照，AI 可直接回答健康指标问题
6. **全链路云监控**：传感器数据经 MQTT 上报至 InfluxDB，前端实时看板展示

---

## 仓库结构

```
guardian-ai-assistant/
├── guardian_esp32_ai/          # ESP32-S3 合并固件（AI语音 + 传感器网关）
│   ├── main/
│   │   ├── sensor_gateway/     # UART接收 → JSON解析 → MQTT发布
│   │   ├── audio/              # I2S麦克风/扬声器驱动、AFE 音频处理、唤醒词框架
│   │   │   ├── codecs/         # NoAudioCodecSimplex（含软件 AEC 参考通道）
│   │   │   ├── processors/     # AfeAudioProcessor（VAD + 神经网络降噪）
│   │   │   └── wake_words/     # AfeWakeWord 框架（待 wakenet 模型）
│   │   ├── protocols/          # WebSocket 小智协议 v3
│   │   └── boards/guardian-s3/ # GPIO引脚定义
│   ├── wifi_config.csv         # WiFi / WebSocket / MQTT 配置（生成NVS bin）
│   └── partitions.csv          # 分区表
│
├── guardian_f407/              # STM32F407 固件（RT-Thread 标准版）
│   ├── applications/           # 业务代码（传感器驱动、跌倒检测、通信）
│   ├── drivers/                # HAL 底层驱动
│   ├── libraries/              # CMSIS / STM32F4xx HAL
│   └── rt-thread/              # RT-Thread 内核源码
│
├── guardian_server/            # 服务端
│   ├── main/xiaozhi-server/    # ASR + LLM + TTS 一体化 AI 语音服务（:8000）
│   └── voiceprint_service/     # 声纹识别服务（resemblyzer，:8002）
│
└── guardian_cloud/             # 云端后端 + 前端看板
    ├── backend/
    │   ├── mqtt_subscriber.py  # MQTT 订阅 + InfluxDB 写入 + 微信推送
    │   ├── api.py              # FastAPI REST API（:8001）
    │   └── .env.example        # 环境变量模板
    └── frontend/
        └── index.html          # Web 监控看板（含声纹注册、AI助手标签页）
```

---

## 系统架构

```
STM32F407（传感器采集）
    │  UART6  PC6→GPIO16  115200 baud
    ▼
ESP32-S3（guardian_esp32_ai 合并固件）
    ├─ sensor_gateway 后台任务 ── MQTT/TLS ──► EMQX Cloud ──► mqtt_subscriber.py ──► InfluxDB
    └─ AI语音助手主任务 ────────── WebSocket ──► xiaozhi-server:8000
                                                     ├─ FunASR（本地 ASR）
                                                     ├─ 阿里云 LLM（qwen-flash）
                                                     ├─ 火山引擎 TTS（双流 WebSocket）
                                                     ├─ 声纹识别 ──► voiceprint_service:8002
                                                     └─ 传感器快照 ──► api.py:8001/sensor-snapshot

InfluxDB ──► api.py:8001 ──► 前端看板:8080
```

> ESP32 只需烧录 `guardian_esp32_ai` 一个固件，传感器网关以后台任务形式集成。

---

## 硬件接线

### F407 ↔ ESP32-S3

| STM32F407 | ESP32-S3 | 说明 |
|-----------|----------|------|
| PC6 (UART6 TX) | GPIO16 | 传感器数据发送 |
| PC7 (UART6 RX) | GPIO17 | NTP 时间同步回传 |
| GND | GND | 共地（必须）|

### ESP32-S3 AI 语音模块

| 模块 | 信号 | GPIO |
|------|------|------|
| INMP441 麦克风 | WS / SCK / DIN | GPIO4 / 5 / 6 |
| MAX98357A 扬声器 | DOUT / BCLK / LRCK | GPIO10 / 11 / 12 |
| MAX98357A SD 使能 | SD | 接 3.3V（常开，不接则无声）|
| BOOT 按键 | — | GPIO0（按下开始/停止对话）|

### STM32F407 传感器

| 传感器 | 接口 | 功能 |
|--------|------|------|
| MPU6050 | I2C | 六轴加速度/陀螺仪，跌倒检测 |
| MAX30102 | I2C | 心率 / 血氧 |
| BME280 | I2C | 温度 / 湿度 / 气压 |
| BH1750 | I2C | 环境光照 |
| MQ-4 | ADC | 天然气泄漏检测 |
| MQ-7 | ADC | CO 一氧化碳检测 |
| ATGM336H | UART | GPS 定位 |
| SSD1306 OLED | I2C | 本地状态显示 |
| SD 卡 | SPI | 本地数据日志 |

---

## 快速启动

### 一键启动（推荐）

```bat
C:\Users\34376\Desktop\embedded_design_competition\docs\项目进程与总结\start_all.bat
```

启动顺序：声纹识别服务（:8002）→ xiaozhi AI 语音（:8000）→ MQTT 订阅 → REST API（:8001）→ 前端（:8080）

### 手动启动（同一 conda 环境 `xiaozhi-esp32-server`）

**窗口 1 — 声纹识别服务**
```powershell
conda activate xiaozhi-esp32-server
cd guardian_server\voiceprint_service
uvicorn app:app --host 0.0.0.0 --port 8002
```

**窗口 2 — AI 语音服务器**
```powershell
conda activate xiaozhi-esp32-server
cd guardian_server\main\xiaozhi-server
python app.py
```

**窗口 3 — MQTT 订阅 + 存储**
```powershell
conda activate xiaozhi-esp32-server
cd guardian_cloud\backend
python mqtt_subscriber.py
```

**窗口 4 — REST API**
```powershell
conda activate xiaozhi-esp32-server
cd guardian_cloud\backend
uvicorn api:app --host 0.0.0.0 --port 8001
```

**窗口 5 — 前端静态服务**
```powershell
cd guardian_cloud\frontend
python -m http.server 8080
```

浏览器访问 `http://localhost:8080`，账号 `admin / 12345`

---

## ESP32 配置烧录

修改 `guardian_esp32_ai/wifi_config.csv` 后重新生成 NVS bin（无需重新编译固件）：

```powershell
# ESP-IDF 专用终端
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py `
    generate wifi_config.csv wifi_nvs.bin 0x6000

esptool.py --chip esp32s3 -p COM12 write_flash 0x9000 wifi_nvs.bin
```

`wifi_config.csv` 关键字段：

| 字段 | 说明 |
|------|------|
| `wifi/ssid` | WiFi 名称（仅 2.4GHz）|
| `wifi/password` | WiFi 密码 |
| `websocket/url` | `ws://电脑IP:8000/xiaozhi/v1/` |
| `mqtt/device_id` | 设备编号（如 `001`）|

---

## 后端服务配置

### AI 模块组合

配置文件 `data/.config.yaml` 优先级高于 `config.yaml`，所有持久化修改写入前者。

| 模块 | 当前配置 | 说明 |
|------|---------|------|
| VAD | SileroVAD | 本地，自动检测说话结束 |
| ASR | FunASR (SenseVoiceSmall) | 本地，model.pt 已下载 |
| LLM | AliyunLLM (qwen-flash-2025-07-28) | 阿里云百炼 API |
| TTS | HuoshanDoubleStreamTTS | 火山引擎双流 WebSocket TTS |
| TTS 音色 | zh_female_wanwanxiaohe_moon_bigtts | 火山引擎温柔女声 |
| Memory | mem_local_short (AliyunLLM) | 短时记忆压缩，LLM 用 AliyunLLM |
| Intent | function_call | 天气、新闻、音乐意图识别 |

### 声纹识别配置

声纹识别服务独立运行于 `guardian_server/voiceprint_service/`：

| 配置项 | 值 | 说明 |
|--------|----|------|
| API Key | `guardian_vp_key` | 通过 query param `?key=` 传入 |
| 相似度阈值 | 0.72 | 低于此值判定为陌生人 |
| 注册端点 | `POST /voiceprint/register` | 支持 `accumulate=true` 累积多次录音取均值 |
| 识别端点 | `POST /voiceprint/identify` | 返回说话人 ID 和相似度分数 |

在前端看板「声纹注册」标签页注册声纹，或直接 curl：
```powershell
# 注册
Invoke-RestMethod -Uri "http://127.0.0.1:8002/voiceprint/register?key=guardian_vp_key" `
  -Method POST -Form @{speaker_id="owner"; file=Get-Item audio.wav}

# 识别测试
Invoke-RestMethod -Uri "http://127.0.0.1:8002/voiceprint/identify?key=guardian_vp_key" `
  -Method POST -Form @{speaker_ids="owner"; file=Get-Item audio.wav}
```

### 云端服务

| 服务 | 地址 |
|------|------|
| EMQX Cloud Broker | `b1117f90.ala.cn-hangzhou.emqxsl.cn:8883`（TLS）|
| InfluxDB Cloud | `us-east-1-1.aws.cloud2.influxdata.com`，Bucket: `guardian` |

### REST API 端点

| 端点 | 说明 |
|------|------|
| `GET /api/heartrate` | 心率 / 血氧数据 |
| `GET /api/env` | 环境数据（温湿度、气体）|
| `GET /api/location` | GPS 位置 |
| `GET /api/alerts` | 告警记录 |
| `GET /api/status` | 系统状态 |
| `GET /api/sensor-snapshot/{device_id}` | AI 实时传感器快照（并发查询 InfluxDB，<300ms）|
| `POST /api/voiceprint/register` | 代理声纹注册（支持 accumulate 累积更新）|
| `POST /api/voiceprint/identify` | 代理声纹识别 |

---

## 端口分配

| 端口 | 服务 |
|------|------|
| 8000 | xiaozhi-esp32-server（WebSocket AI 语音）|
| 8001 | api.py（传感器数据 REST API）|
| 8002 | voiceprint_service（声纹识别 HTTP REST）|
| 8080 | 前端看板（HTTP）|

---

## 当前开发状态

### 已完成

- [x] STM32F407 多线程固件框架（9 个传感器驱动 + 跌倒检测状态机 + UART 协议）
- [x] ESP32-S3 合并固件（AI 语音助手 + 传感器网关二合一）
- [x] 小智协议 v3 WebSocket 通信
- [x] FunASR 本地语音识别部署
- [x] 火山引擎 TTS 双流 WebSocket（低延迟，温柔女声）
- [x] MQTT 云端数据链路（ESP32 → EMQX → InfluxDB）
- [x] 前端看板（数据概览 / 实时图表 / GPS 地图 / AI 助手 / 声纹注册 / 告警历史）
- [x] 声纹识别服务（resemblyzer，注册/识别/累积更新）
- [x] AI 实时感知传感器数据（连接时注入 + 每次对话前刷新）
- [x] ESP32 AFE 音频处理器（VAD + 神经网络降噪）
- [x] 软件 AEC 参考通道（menuconfig 可选）
- [x] 唤醒词检测框架（代码完整，待配置 wakenet 模型）
- [x] 短时记忆模块（mem_local_short + AliyunLLM）
- [x] 一键启动脚本（含声纹服务）

### 进行中 / 待完成

- [ ] wakenet 模型配置（idf_component.yml 引入，实现免按键唤醒）
- [ ] AEC 实机效果验证（menuconfig → Enable Device-Side AEC）
- [ ] 声纹注册用 ESP32 录音（解决浏览器 WebM vs PCM 跨设备域偏移问题）
- [ ] STM32F407 传感器硬件实物调试
- [ ] K230 RT-Thread Smart 环境搭建（等待硬件到货）
- [ ] YOLO-Pose 跌倒检测模型 KPU 移植
- [ ] 告警主动联动 AI（mqtt_subscriber → AI WebSocket 推送系统消息）
- [ ] 三板完整联调测试

---

## F407 MSH 调试命令

| 命令 | 功能 |
|------|------|
| `esp_test` | 发送测试数据至 ESP32 |
| `esp_alert fall` | 触发跌倒告警 |
| `esp_alert gas` | 触发燃气告警 |

---

## 常见问题

**前端传感器数据不显示**：确认 `api.py` 运行在 8001 端口，前端 `API_BASE` 指向 `http://127.0.0.1:8001`。

**AI 助手显示"连接中"**：确认 `xiaozhi-server` 已启动；前端必须通过 `http://localhost:8080` 访问，不能直接打开文件。

**ESP32 MQTT 断开**：检查手机热点是否为 2.4GHz；串口日志 `Config loaded:` 行确认 SSID/密码。

**TTS 无声音**：MAX98357A 的 SD 引脚必须接 3.3V；确认火山引擎 TTS 的 access_token 未过期（`data/.config.yaml` → `TTS.HuoshanDoubleStreamTTS.access_token`）。

**修改配置不生效**：持久化配置写入 `data/.config.yaml`（优先级高于 `config.yaml`）。

**收不到 F407 数据**：检查 UART 接线 PC6→GPIO16 / PC7→GPIO17 / GND→GND，波特率 115200。

**声纹识别总返回 stranger**：先检查声纹服务是否在 8002 端口运行；相似度阈值默认 0.72，可在 `data/.config.yaml` → `voiceprint.similarity_threshold` 调低；跨设备录音（浏览器 vs ESP32）会导致分数偏低，建议用「累积更新」多次注册。

**声纹服务启动报 lzma DLL 错误**：已在 `pooch/processors.py` 中修复（try/except），若重装包后复现，重新应用该补丁。

**传感器快照刷新超时警告**：InfluxDB 查询正常情况 <300ms；若频繁超时检查 InfluxDB token 是否有效（`guardian_cloud/backend/.env`）。

---

## License

项目代码采用 MIT License。`guardian_server/` 基于 [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 开发，遵循其原始许可证。

---

*竞赛：全国大学生嵌入式芯片与系统设计竞赛 2026*
*最后更新：2026-04-06*
