#pragma once
/**
 * Sensor Gateway — UART(F407) → JSON → MQTT
 *
 * 从 guardian_esp32 移植的传感器网关逻辑，以独立 FreeRTOS 任务形式
 * 集成到 guardian_esp32_ai 固件中，与 AI 语音助手共用同一块 ESP32-S3。
 *
 * 使用方式：
 *   在 WiFi 连通后调用 sensor_gateway_start()，该函数会：
 *     1. 从 NVS mqtt 命名空间读取 device_id（fallback 到 Kconfig 默认值）
 *     2. 初始化 UART1（GPIO16 RX / GPIO17 TX）
 *     3. 连接 EMQX Cloud MQTT Broker（TLS，后台自动重连）
 *     4. 启动 uart_rx_task，持续接收 F407 数据并发布到 MQTT
 *     5. 启动 NTP 时间同步，同步后将时间发送给 F407
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化并启动传感器网关（在 WiFi 连接成功后调用一次）
 *         该函数会新建后台任务，不阻塞调用者。
 */
void sensor_gateway_start(void);

#ifdef __cplusplus
}
#endif
