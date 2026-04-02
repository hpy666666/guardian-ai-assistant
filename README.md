# Guardian AI Assistant

面向独居老人的智能守护系统，集成健康监测、AI语音助手与异常预警功能。

## 项目结构

```
guardian-ai-assistant/
├── guardian_esp32_ai/      # ESP32-S3 AI语音助手固件（ESP-IDF）
├── guardian_f407/          # STM32F407 主控固件（RT-Thread）
├── guardian_cloud/
│   ├── backend/            # 云端后端：MQTT订阅、InfluxDB存储、微信推送
│   └── frontend/           # 数据可视化 Web 面板
├── guardian_server/        # AI语音服务端（基于 xiaozhi-esp32-server）
│   └── main/xiaozhi-server/
└── docs/
    ├── 项目进程与总结/       # 开发日志与总结
    ├── 模块资料/             # 传感器模块参考资料
    └── archive/             # 归档的旧版代码
```

## 系统架构

```
传感器 (MPU6050/MAX30102)
    → STM32F407 (RT-Thread)
        → MQTT (EMQX Cloud)
            → 云端后端 (Python)
                → InfluxDB (时序数据存储)
                → 微信推送 (异常报警)
                → Web 面板 (数据可视化)

用户语音
    → ESP32-S3
        → WebSocket
            → AI 语音服务端
                → LLM (通义千问/DeepSeek)
                → TTS (IndexTTS / 阿里云)
```

## 快速开始

### 云端后端

```bash
cd guardian_cloud/backend
cp .env.example .env
# 编辑 .env，填入你的 API Key
pip install -r requirements.txt
python mqtt_subscriber.py
```

### AI 语音服务端

```bash
cd guardian_server/main/xiaozhi-server
cp data/.config.yaml.example data/.config.yaml
# 编辑 .config.yaml，填入你的 API Key
pip install -r requirements.txt
python app.py
```

### ESP32 固件

参考 `guardian_esp32_ai/` 目录下的 `README.md`，使用 ESP-IDF v5.x 编译烧录。

### STM32 固件

使用 RT-Thread Studio 或 Keil 打开 `guardian_f407/` 工程编译烧录。

## 配置说明

项目使用两个配置文件（均不纳入版本管理）：

| 文件 | 模板 | 说明 |
|------|------|------|
| `guardian_cloud/backend/.env` | `.env.example` | MQTT、InfluxDB、API Key |
| `guardian_server/main/xiaozhi-server/data/.config.yaml` | `.config.yaml.example` | LLM、TTS、ASR 配置 |

## 硬件清单

- STM32F407 开发板
- ESP32-S3 开发板（带麦克风）
- MPU6050 六轴传感器（跌倒检测）
- MAX30102 脉氧传感器（心率/血氧）
- MQ-2 烟雾传感器

## License

本项目代码采用 MIT License。`guardian_server/` 基于 [xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 开发。
