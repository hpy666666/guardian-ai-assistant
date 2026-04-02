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
3. **AI 语音助手**：基于 xiaozhi-esp32-server，集成 FunASR（本地）+ 阿里云 LLM + IndexTTS（本地），全程低延迟
4. **全链路云监控**：传感器数据经 MQTT 上报至 InfluxDB，前端实时看板展示

---

## 仓库结构

```
guardian-ai-assistant/
├── guardian_esp32_ai/          # ESP32-S3 合并固件（AI语音 + 传感器网关）
│   ├── main/
│   │   ├── sensor_gateway/     # UART接收 → JSON解析 → MQTT发布
│   │   ├── audio/              # I2S麦克风/扬声器驱动
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
├── guardian_server/            # 服务端 AI 管道（xiaozhi-esp32-server）
│   └── main/xiaozhi-server/    # ASR + LLM + TTS 一体化服务
│
└── guardian_cloud/             # 云端后端 + 前端看板
    ├── backend/
    │   ├── mqtt_subscriber.py  # MQTT 订阅 + InfluxDB 写入 + 微信推送
    │   ├── api.py              # FastAPI REST API（:8001）
    │   └── .env.example        # 环境变量模板
    └── frontend/
        └── index.html          # Web 监控看板（含AI助手标签页）
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
                                                     ├─ 阿里云 LLM（qwen）
                                                     └─ IndexTTS（WSL2，:11996）

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
C:\Users\34376\Desktop\start_all.bat
```

启动顺序：IndexTTS（WSL2）→ MQTT订阅 → REST API → xiaozhi-server → 前端

### 手动启动

**窗口 1 — IndexTTS（WSL2）**
```bash
# WSL2 Ubuntu 内
conda activate index-tts-vllm
cd /home/hpy/index-tts-vllm
CC=/usr/bin/gcc python api_server.py \
    --model_dir /mnt/d/models/IndexTeam/IndexTTS-1___5 \
    --port 11996 --gpu_memory_utilization 0.85
```

**窗口 2 — MQTT 订阅 + 存储**
```powershell
cd guardian_cloud\backend
python mqtt_subscriber.py
```

**窗口 3 — REST API**
```powershell
cd guardian_cloud\backend
uvicorn api:app --host 0.0.0.0 --port 8001
```

**窗口 4 — AI 语音服务器**
```powershell
cd guardian_server\main\xiaozhi-server
python app.py
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
| LLM | AliyunLLM (qwen-flash) | 阿里云百炼 API |
| TTS | IndexStreamTTS | 本地 IndexTTS（WSL2 :11996）|
| TTS 音色 | nahida | 原神纳西妲女声（参考音频 7s）|

切换音色：修改 `data/.config.yaml` 中 `TTS.IndexStreamTTS.voice`，重启 xiaozhi-server。

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

---

## 端口分配

| 端口 | 服务 |
|------|------|
| 8000 | xiaozhi-esp32-server（WebSocket AI 语音）|
| 8001 | api.py（传感器数据 REST API）|
| 8080 | 前端看板（HTTP）|
| 11996 | IndexTTS（WSL2 HTTP REST）|

---

## 当前开发状态

### 已完成

- [x] STM32F407 多线程固件框架（9 个传感器驱动 + 跌倒检测状态机 + UART 协议）
- [x] ESP32-S3 合并固件（AI 语音助手 + 传感器网关二合一）
- [x] 小智协议 v3 WebSocket 通信
- [x] FunASR 本地语音识别部署
- [x] IndexTTS（WSL2）本地 TTS，含三个音色（nahida / xu_sheng / xiao_he）
- [x] TTS 音频失真修复（24kHz→16kHz 降采样 + end_of_stream flush）
- [x] MQTT 云端数据链路（ESP32 → EMQX → InfluxDB）
- [x] 前端看板（数据概览 / 实时图表 / GPS 地图 / AI 助手 / 告警历史）
- [x] 一键启动脚本

### 进行中 / 待完成

- [ ] INMP441 麦克风接线确认（L/R 引脚需接 GND）
- [ ] STM32F407 传感器硬件实物调试
- [ ] K230 RT-Thread Smart 环境搭建（等待硬件到货）
- [ ] YOLO-Pose 跌倒检测模型 KPU 移植
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

**TTS 无声音**：MAX98357A 的 SD 引脚必须接 3.3V；确认 IndexTTS 在 WSL2 中正常运行（日志出现 `Uvicorn running on http://0.0.0.0:11996`）。

**修改配置不生效**：持久化配置写入 `data/.config.yaml`（优先级高于 `config.yaml`）。

**收不到 F407 数据**：检查 UART 接线 PC6→GPIO16 / PC7→GPIO17 / GND→GND，波特率 115200。

---

## License

项目代码采用 MIT License。`guardian_server/` 基于 [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 开发，遵循其原始许可证。

---

*竞赛：全国大学生嵌入式芯片与系统设计竞赛 2026*
*最后更新：2026-04-02*
