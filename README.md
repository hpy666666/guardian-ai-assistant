# Guardian — 智能安全监护系统

面向**独居老人和残障人士**的多平台智能监护系统。整合视觉 AI、语音交互、多路传感器与云端看板，覆盖跌倒检测、反诈骗监听、生理健康监测、AI 语音助手、家属实时告警等场景。

> 全国大学生嵌入式芯片与系统设计竞赛 2026 · 睿赛德赛道

---

## 系统架构

```
STM32F407（传感器 + IMU）
        │ UART 115200
        ▼
ESP32-S3（AI 语音 + MQTT 网关）
        │ WiFi / EMQX Cloud TLS
        ▼
EMQX Cloud Broker（TLS 8883）
        │
        ├──► RK3576 泰山派（摄像头视觉 AI，独立 WiFi 接入）
        │
        └──► PC 云端
               ├── mqtt_subscriber.py → InfluxDB Cloud
               ├── api.py :8001（REST API）
               ├── xiaozhi-esp32-server :8010（AI 语音后端）
               ├── voiceprint_service :8002（声纹识别）
               ├── funasr_http_service.py :10097（本地 ASR）
               └── index.html :8080（前端看板）
```

### 硬件平台

| 板卡 | 芯片 | 主要职责 |
|------|------|---------|
| 主控板 | STM32F407ZGT6 | 传感器采集、IMU 跌倒检测、SD 卡日志 |
| AI 语音板 | ESP32-S3 N16R8 | 语音对话、WiFi 联网、MQTT 上云、传感器网关 |
| 视觉板 | RK3576（泰山派） | 摄像头跌倒检测、人脸识别、目标检测、MJPEG 推流 |
| 云端 PC | Windows 11 | AI 后端、InfluxDB、前端看板、声纹识别 |

---

## 两种工作模式

### 放置模式（摄像头固定）

- 摄像头视野稳定，持续监测居家环境
- **视觉跌倒检测**：YOLOv8n-pose 姿态估计 + 速度特征 + 三阶段状态机（NPU Core 0，~20 fps）
- **人脸识别**：SCRFD-500M-KPS + MobileNet ArcFace，识别家庭成员；陌生人触发告警
- **反诈骗监听**：检测到陌生人时自动开启麦克风监听，AI 分析对话内容，MQTT 上报

### 携带模式（随身携带）

- 摄像头随身晃动，停用视觉跌倒检测
- **IMU 跌倒检测**：MPU6050 三轴加速度，三阶段状态机（自由落体 → 撞击 → 静止确认）
- **目标检测辅助**：YOLOv8n-det（NPU Core 2），辅助视障人士识别障碍物、红绿灯等
- GPS 实时轨迹 + AI 导航辅助

---

## 功能概览

### 视觉感知（RK3576）

| 功能 | 状态 |
|------|------|
| 摄像头跌倒检测（放置模式） | 已完成 |
| 人脸检测 + 识别（SCRFD + ArcFace） | 已完成 |
| 身份锁定缓存（track_id，恢复 FPS ~20） | 已完成 |
| MJPEG 视频推流（前端直接接入） | 已完成 |
| 陌生人告警 + 反诈骗监听 | 已完成 |
| 携带模式目标检测（YOLOv8n-det） | 已完成 |
| 网页人脸录入接口（/enroll、/face_db） | 已完成 |
| 告警截图存档（含时间戳和姓名） | 已完成 |

### AI 语音交互（ESP32-S3 + xiaozhi-server）

| 功能 | 状态 |
|------|------|
| 按键唤醒（BOOT 键） | 已完成 |
| 语音识别 ASR（FunASR SenseVoiceSmall，本地） | 已完成 |
| 大模型对话（qwen-flash，function_call 工具调用） | 已完成 |
| 语音合成 TTS（火山引擎双流 WSS，边生成边播放） | 已完成 |
| 噪声抑制（AFE NSNet 神经网络降噪） | 已完成 |
| 传感器数据注入 LLM（AI 可回答"我心率多少"） | 已完成 |
| 唤醒词检测（WakeNet，框架就绪） | 基本完成 |
| 回声消除 AEC（框架就绪，待实机验证） | 待验证 |

### 生理健康监测（STM32F407）

MAX30102 心率 + 血氧 / BME280 温湿度气压 / MQ-4 甲烷 / MQ-7 一氧化碳 / BH1750 光照 / ATGM336H GPS

### 运动监测（STM32F407）

MPU6050 姿态解算 + 三阶段跌倒状态机（携带模式）

### 云端平台（PC）

| 功能 | 状态 |
|------|------|
| MQTT TLS 接收（EMQX Cloud 8883） | 已完成 |
| InfluxDB 时序存储（全传感器 + 告警） | 已完成 |
| 微信告警推送（Server酱，60s 冷却） | 已完成 |
| REST API（FastAPI :8001，历史查询 + 快照） | 已完成 |
| 声纹识别服务（resemblyzer 256 维，:8002） | 已完成 |
| 传感器快照注入 AI 上下文（/api/sensor-snapshot） | 已完成 |

### 前端看板（Web :8080）

| 页面 | 内容 |
|------|------|
| 数据概览 | 心率/血氧/温湿度/气体实时卡片 + 折线图（支持 15分/1h/6h/24h） |
| 告警历史 | 告警列表，支持类型过滤 |
| GPS 地图 | Leaflet + OpenStreetMap，实时轨迹 |
| 视觉监控 | MJPEG 视频流 + AI 推理状态 + 跌倒截图面板 |
| AI 助手 | TTS 音色/Prompt/记忆管理 + WebSocket 文字对话测试 |
| 声纹注册 | 浏览器录音 / 上传注册 + 说话人管理 |
| 人脸库管理 | 拖拽上传录入、人脸列表、删除（对应泰山派 /enroll 接口） |

---

## 目录结构

```
embedded_design_competition/
├── guardian_f407/              # STM32F407 RT-Thread 固件
│   └── applications/           # 业务代码（传感器、跌倒检测、OLED、LED）
├── guardian_esp32_ai/          # ESP32-S3 ESP-IDF 固件
│   ├── main/                   # 应用代码（音频、WebSocket、传感器网关）
│   └── wifi_config.csv         # WiFi / WebSocket 地址配置（修改后生成 NVS bin 烧录）
├── guardian_taishanpi/         # RK3576 泰山派视觉 AI
│   └── rknn/
│       ├── zerocopy/
│       │   └── pipeline_zerocopy.py  # 主推理流水线（放置模式）
│       ├── pipeline_carry.py         # 携带模式目标检测
│       └── face_db/                  # 已注册人脸特征库
├── guardian_server/            # PC 端 AI 语音服务
│   ├── main/xiaozhi-server/    # xiaozhi-esp32-server（第三方，含二次开发）
│   │   └── data/.config.yaml   # 运行时配置（TTS/ASR/LLM/声纹）
│   ├── voiceprint_service/     # 声纹识别服务（:8002）
│   └── funasr_http_service.py  # FunASR HTTP 服务（:10097）
├── guardian_cloud/             # PC 端云端服务 + 前端
│   ├── backend/
│   │   ├── api.py              # REST API（:8001）
│   │   └── mqtt_subscriber.py  # MQTT 订阅 + InfluxDB + 微信推送
│   └── frontend/
│       └── index.html          # 单文件前端看板
├── start_all.bat               # 一键启动脚本（Windows）
└── docs/                       # 开发文档（gitignore，本地保留）
```

---

## 快速启动

### 前置条件

- Windows 11 + Python 3.12 conda 环境 `xiaozhi-esp32-server`
- InfluxDB Cloud 账号（免费版）
- EMQX Cloud TLS Broker（免费版）
- 火山引擎 TTS AppID + Access Token
- 阿里云百炼 API Key（qwen-flash）
- ffmpeg 已加入系统 PATH（声纹服务依赖）

### 配置文件

**`guardian_server/main/xiaozhi-server/data/.config.yaml`** — AI 服务核心配置（TTS Token、LLM Key、声纹服务地址、WebSocket IP）

**`guardian_cloud/backend/.env`**（从 `.env.example` 复制）：
```
MQTT_BROKER=<EMQX Cloud 地址>
MQTT_PORT=8883
INFLUX_URL=https://...influxdata.com
INFLUX_TOKEN=...
INFLUX_ORG=...
INFLUX_BUCKET=guardian
SERVERCHAN_KEY=...   # 微信推送，可选
```

### 修改服务器 IP

编辑 `start_all.bat` 顶部两行：
```bat
set TAISHAN_IP=192.168.1.26   ← 泰山派实际 IP
set PC_IP=192.168.1.16        ← 本机实际 IP
```
脚本会自动更新 `.config.yaml` 中的 WebSocket 地址。

### 启动所有服务

```bat
start_all.bat
```

启动顺序：声纹服务（:8002）→ MQTT 订阅 → REST API（:8001）→ AI 语音（:8010）→ 前端（:8080）→ FunASR（:10097）

浏览器访问：`http://localhost:8080`

### 端口分配

| 端口 | 服务 |
|------|------|
| 8001 | REST API（FastAPI） |
| 8002 | 声纹识别服务 |
| 8010 | xiaozhi-esp32-server（AI 语音 WebSocket） |
| 8080 | 前端看板（静态文件） |
| 10097 | FunASR HTTP 服务（本地 ASR） |

---

## 硬件配置

### ESP32-S3 WiFi / 服务器地址更新

修改 `guardian_esp32_ai/wifi_config.csv`，重新生成并烧录 NVS（无需重新编译固件）：

```bash
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
    generate wifi_config.csv wifi_nvs.bin 0x6000

esptool.py --chip esp32s3 --port COM12 write_flash 0x9000 wifi_nvs.bin
```

### 泰山派 FunASR 地址更新

```bash
ssh lckfb@<TAISHAN_IP> "sed -i 's|http://[0-9.]*:10097/asr|http://<PC_IP>:10097/asr|g' ~/rknn/zerocopy/pipeline_zerocopy.py"
```

### 人脸录入

通过前端"人脸库管理"页面上传，或直接调用接口：

```bash
# 录入
curl -X POST http://<TAISHAN_IP>:8080/enroll -F "name=张三" -F "file=@face.jpg"
# 查看
curl http://<TAISHAN_IP>:8080/face_db
# 删除
curl -X DELETE http://<TAISHAN_IP>:8080/face_db/张三
```

### 声纹注册

通过前端"声纹注册"页面录音上传，或离线脚本注册：

```bash
python guardian_server/voiceprint_service/register.py <音频.wav>
```

---

## 技术栈

| 层 | 技术 |
|----|------|
| 嵌入式固件 | RT-Thread（STM32F407）/ ESP-IDF v5.5.1（ESP32-S3） |
| 视觉 AI | RKNNLite NPU，YOLOv8n-pose / SCRFD / ArcFace / YOLOv8n-det |
| 语音 AI | FunASR SenseVoiceSmall（ASR）/ qwen-flash（LLM）/ 火山引擎 HuoshanDoubleStreamTTS |
| 音频协议 | Opus 60ms 帧（DTX + VBR）/ OGG 解封装 / WebSocket 二进制 v3 |
| 数据存储 | InfluxDB Cloud（时序）/ JSON Lines（SD 卡日志） |
| 消息传输 | EMQX Cloud MQTT TLS 8883 |
| 后端 | FastAPI Python 3.12 / paho-mqtt / resemblyzer（声纹）|
| 前端 | 原生 HTML/JS / ECharts 5 / Leaflet 1.9 |

---

## 待完成事项

### 高优先级

- **J8** 告警联动 AI 主动播报：mqtt_subscriber 收到跌倒告警后向 ESP32 推送语音提醒
- **G1** 唤醒词模型配置：引入 WakeNet 模型，实现免按键唤醒

### 中优先级

- **H2** 打电话 / 发微信语音指令
- **C3** IMU 跌倒阈值实测调参
- **E4** 灯带告警联动（跌倒→红闪 / 气体→黄闪）

### 低优先级

- G10 对话记忆持久化（重启后清空）
- J9 InfluxDB 数据降采样（>6h 图表）
- L5 公网穿透稳定化（ngrok + WSL2）

---

*最后更新：2026-04-30*
