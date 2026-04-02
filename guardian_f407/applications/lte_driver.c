/*
 * Air780E 4G LTE Module Driver Implementation
 *
 * !! DISABLED !!
 * Reason: Air780E AT firmware does not support MQTT over TLS.
 *   - AT+CMQTTSTART → "command not found" (no CMQTT command set)
 *   - AT+MIPSTART to port 8883 → +CME ERROR: 767 (TCP rejected by TLS port)
 * This entire file is excluded from compilation via #if 0 below.
 *
 * AT Command Flow (Air780E specific):
 *   1. AT          - Test communication
 *   2. ATE0        - Disable echo
 *   3. AT+CPIN?    - Check SIM card
 *   4. AT+COPS?    - Get operator (for APN auto-detect)
 *   5. AT+CSQ      - Check signal quality
 *   6. AT+CREG?    - Check network registration
 *   7. AT+CGATT=1  - Attach to GPRS
 *   8. AT+CGDCONT  - Set APN (auto-detected)
 *   9. AT+CGACT    - Activate PDP context
 *  10. AT+MCONFIG  - Configure MQTT client
 *  11. AT+MIPSTART - Open TCP/SSL connection to broker
 *  12. AT+MCONNECT - Send MQTT CONNECT packet
 *  13. AT+MPUB     - Publish message
 *  14. AT+MDISCONNECT - Disconnect MQTT
 *  15. AT+MIPCLOSE - Close TCP connection
 */

/* ======================================================================
 * ENTIRE FILE DISABLED - see header comment for reason
 * ====================================================================== */
#if 0

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lte_driver.h"

#define DBG_TAG "lte"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/*---------------------------------------------------------------------------
 * Private Variables
 *---------------------------------------------------------------------------*/
static rt_device_t s_uart_dev = RT_NULL;
static rt_sem_t s_rx_sem = RT_NULL;
static lte_status_t s_status = {0};
static rt_bool_t s_initialized = RT_FALSE;

/* Runtime configuration */
static lte_config_t s_config = {
    .broker = LTE_MQTT_BROKER_DEFAULT,
    .port = LTE_MQTT_PORT_DEFAULT,
    .username = LTE_MQTT_USERNAME_DEFAULT,
    .password = LTE_MQTT_PASSWORD_DEFAULT,
    .client_id = LTE_MQTT_CLIENT_ID_DEFAULT,
    .device_id = LTE_MQTT_DEVICE_ID_DEFAULT,
    .apn = ""  /* Empty = auto-detect */
};

/* TX buffer for commands */
#define TX_BUF_SIZE     512     /* Increased from 256 */
static char s_tx_buf[TX_BUF_SIZE];

/* MQTT topic buffer */
#define TOPIC_BUF_SIZE  64
static char s_topic_buf[TOPIC_BUF_SIZE];

/* Reconnection thread handle */
static rt_thread_t s_reconnect_thread = RT_NULL;
static rt_bool_t s_reconnect_requested = RT_FALSE;

/*---------------------------------------------------------------------------
 * UART RX Callback
 *---------------------------------------------------------------------------*/
static rt_err_t uart_rx_callback(rt_device_t dev, rt_size_t size)
{
    /* Signal that data is available */
    if (s_rx_sem) {
        rt_sem_release(s_rx_sem);
    }
    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Read UART Response (improved parsing)
 *---------------------------------------------------------------------------*/
static rt_size_t read_response(char *buf, rt_size_t max_len, rt_uint32_t timeout)
{
    rt_tick_t start = rt_tick_get();
    rt_size_t total = 0;
    char ch;
    rt_bool_t found_end = RT_FALSE;

    while (rt_tick_get() - start < rt_tick_from_millisecond(timeout)) {
        if (rt_device_read(s_uart_dev, 0, &ch, 1) == 1) {
            if (total < max_len - 1) {
                buf[total++] = ch;
            }

            /* Check for complete response markers
             * Must end with \r\n after OK/ERROR, or have "> " prompt */
            if (total >= 4) {
                /* Look for "OK\r\n" at end */
                if (buf[total-4] == 'O' && buf[total-3] == 'K' &&
                    buf[total-2] == '\r' && buf[total-1] == '\n') {
                    found_end = RT_TRUE;
                    break;
                }
                /* Look for "ERROR\r\n" at end */
                if (total >= 7 &&
                    strncmp(&buf[total-7], "ERROR\r\n", 7) == 0) {
                    found_end = RT_TRUE;
                    break;
                }
            }
            /* Check for data input prompt "> " */
            if (total >= 2 && buf[total-2] == '>' && buf[total-1] == ' ') {
                found_end = RT_TRUE;
                break;
            }
        } else {
            /* Wait for data with semaphore */
            if (rt_sem_take(s_rx_sem, rt_tick_from_millisecond(50)) != RT_EOK) {
                /* Timeout waiting for more data */
                if (total > 0 && !found_end) {
                    /* Already have some data, wait a bit more */
                    rt_thread_mdelay(20);
                    continue;
                }
            }
        }
    }

    buf[total] = '\0';
    return total;
}

/*---------------------------------------------------------------------------
 * Send AT Command
 *---------------------------------------------------------------------------*/
rt_err_t lte_send_at(const char *cmd, char *resp, rt_size_t resp_size, rt_uint32_t timeout)
{
    if (!s_initialized || !s_uart_dev) {
        return -RT_ERROR;
    }

    /* Clear RX buffer */
    char dummy;
    while (rt_device_read(s_uart_dev, 0, &dummy, 1) > 0);

    /* Build command in a separate local buffer to avoid conflict
     * when cmd points to s_tx_buf (same buffer reuse issue) */
    char at_cmd[256];
    int len = snprintf(at_cmd, sizeof(at_cmd), "AT%s\r\n", cmd);

    /* Now safe to copy to TX buffer and send */
    rt_device_write(s_uart_dev, 0, at_cmd, len);

    LOG_D("TX: AT%s", cmd);

    /* Read response */
    rt_size_t rx_len = read_response(resp, resp_size, timeout);

    if (rx_len > 0) {
        LOG_D("RX: %s", resp);

        if (strstr(resp, "OK")) {
            return RT_EOK;
        } else if (strstr(resp, "ERROR")) {
            return -RT_ERROR;
        }
    }

    return -RT_ETIMEOUT;
}

/*---------------------------------------------------------------------------
 * Send AT Command (simple version without response)
 *---------------------------------------------------------------------------*/
static rt_err_t send_at_simple(const char *cmd, rt_uint32_t timeout)
{
    char resp[128];
    return lte_send_at(cmd, resp, sizeof(resp), timeout);
}

/*---------------------------------------------------------------------------
 * Initialize LTE Module
 *---------------------------------------------------------------------------*/
rt_err_t lte_init(void)
{
    if (s_initialized) {
        return RT_EOK;
    }

    /* Create semaphore for RX */
    s_rx_sem = rt_sem_create("lte_rx", 0, RT_IPC_FLAG_FIFO);
    if (s_rx_sem == RT_NULL) {
        LOG_E("Failed to create RX semaphore");
        return -RT_ENOMEM;
    }

    /* Find UART device */
    s_uart_dev = rt_device_find(LTE_UART_NAME);
    if (s_uart_dev == RT_NULL) {
        LOG_E("UART '%s' not found", LTE_UART_NAME);
        return -RT_ENOSYS;
    }

    /* Configure UART */
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;
    config.baud_rate = LTE_UART_BAUDRATE;
    config.data_bits = DATA_BITS_8;
    config.stop_bits = STOP_BITS_1;
    config.parity    = PARITY_NONE;
    rt_device_control(s_uart_dev, RT_DEVICE_CTRL_CONFIG, &config);

    /* Open UART with interrupt mode */
    rt_err_t ret = rt_device_open(s_uart_dev, RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_INT_TX);
    if (ret != RT_EOK) {
        LOG_E("Failed to open UART");
        return ret;
    }

    /* Set RX callback */
    rt_device_set_rx_indicate(s_uart_dev, uart_rx_callback);

    /* Clear any stale data in RX buffer (module may send boot messages) */
    rt_thread_mdelay(100);  /* Wait for boot messages */
    char dummy;
    while (rt_device_read(s_uart_dev, 0, &dummy, 1) > 0) {
        /* Drain buffer */
    }

    s_initialized = RT_TRUE;
    s_status.state = LTE_STATE_INIT;

    LOG_I("LTE driver initialized on %s @ %d baud", LTE_UART_NAME, LTE_UART_BAUDRATE);
    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Configuration Functions
 *---------------------------------------------------------------------------*/
void lte_set_config(const lte_config_t *config)
{
    if (config == RT_NULL) {
        /* Reset to defaults */
        strncpy(s_config.broker, LTE_MQTT_BROKER_DEFAULT, sizeof(s_config.broker) - 1);
        s_config.port = LTE_MQTT_PORT_DEFAULT;
        strncpy(s_config.username, LTE_MQTT_USERNAME_DEFAULT, sizeof(s_config.username) - 1);
        strncpy(s_config.password, LTE_MQTT_PASSWORD_DEFAULT, sizeof(s_config.password) - 1);
        strncpy(s_config.client_id, LTE_MQTT_CLIENT_ID_DEFAULT, sizeof(s_config.client_id) - 1);
        strncpy(s_config.device_id, LTE_MQTT_DEVICE_ID_DEFAULT, sizeof(s_config.device_id) - 1);
        s_config.apn[0] = '\0';
    } else {
        memcpy(&s_config, config, sizeof(lte_config_t));
    }
    LOG_I("LTE config updated: broker=%s, device=%s", s_config.broker, s_config.device_id);
}

const lte_config_t* lte_get_config(void)
{
    return &s_config;
}

/*---------------------------------------------------------------------------
 * Auto-detect APN based on operator
 *---------------------------------------------------------------------------*/
static const char* detect_apn(void)
{
    char resp[128];
    rt_err_t ret;

    /* If APN is configured, use it */
    if (s_config.apn[0] != '\0') {
        LOG_I("Using configured APN: %s", s_config.apn);
        return s_config.apn;
    }

    /* Query operator: AT+COPS? */
    ret = lte_send_at("+COPS?", resp, sizeof(resp), LTE_CMD_TIMEOUT);
    if (ret == RT_EOK) {
        /* Response format: +COPS: 0,0,"CHINA MOBILE",7 */
        if (strstr(resp, "MOBILE") || strstr(resp, "CMCC") || strstr(resp, "46000") || strstr(resp, "46002")) {
            strncpy(s_status.operator_name, "CHINA MOBILE", sizeof(s_status.operator_name) - 1);
            LOG_I("Operator: China Mobile, using APN: %s", LTE_APN_CMNET);
            return LTE_APN_CMNET;
        }
        if (strstr(resp, "UNICOM") || strstr(resp, "46001") || strstr(resp, "46006")) {
            strncpy(s_status.operator_name, "CHINA UNICOM", sizeof(s_status.operator_name) - 1);
            LOG_I("Operator: China Unicom, using APN: %s", LTE_APN_UNINET);
            return LTE_APN_UNINET;
        }
        if (strstr(resp, "TELECOM") || strstr(resp, "46003") || strstr(resp, "46005") || strstr(resp, "46011")) {
            strncpy(s_status.operator_name, "CHINA TELECOM", sizeof(s_status.operator_name) - 1);
            LOG_I("Operator: China Telecom, using APN: %s", LTE_APN_CTNET);
            return LTE_APN_CTNET;
        }
    }

    /* Default to CMNET if detection fails */
    LOG_W("Operator detection failed, defaulting to CMNET");
    strncpy(s_status.operator_name, "UNKNOWN", sizeof(s_status.operator_name) - 1);
    return LTE_APN_CMNET;
}

/*---------------------------------------------------------------------------
 * Connect to Network and MQTT
 *---------------------------------------------------------------------------*/
rt_err_t lte_connect(void)
{
    char resp[256];
    rt_err_t ret;
    const char *apn;

    if (!s_initialized) {
        ret = lte_init();
        if (ret != RT_EOK) return ret;
    }

    LOG_I("Starting LTE connection...");
    s_status.reconnect_count++;

    /* Step 1: Test AT communication */
    LOG_I("[1/11] Testing AT communication...");
    for (int i = 0; i < 5; i++) {
        ret = send_at_simple("", LTE_CMD_TIMEOUT);
        if (ret == RT_EOK) break;
        rt_thread_mdelay(1000);
    }
    if (ret != RT_EOK) {
        LOG_E("AT communication failed");
        s_status.state = LTE_STATE_ERROR;
        return -RT_ERROR;
    }

    /* Step 2: Disable echo */
    LOG_I("[2/11] Disabling echo...");
    send_at_simple("E0", LTE_CMD_TIMEOUT);

    /* Step 3: Check SIM card */
    LOG_I("[3/11] Checking SIM card...");
    ret = lte_send_at("+CPIN?", resp, sizeof(resp), LTE_CMD_TIMEOUT);
    if (ret != RT_EOK || !strstr(resp, "READY")) {
        LOG_E("SIM card not ready");
        s_status.state = LTE_STATE_ERROR;
        return -RT_ERROR;
    }
    s_status.sim_ready = RT_TRUE;
    s_status.state = LTE_STATE_SIM_READY;
    LOG_I("  SIM card ready");

    /* Step 4: Check signal quality */
    LOG_I("[4/11] Checking signal quality...");
    s_status.signal_quality = lte_get_signal_quality();
    LOG_I("  Signal: %d", s_status.signal_quality);

    /* Step 5: Wait for network registration */
    LOG_I("[5/11] Waiting for network registration...");
    rt_tick_t start = rt_tick_get();
    while (rt_tick_get() - start < rt_tick_from_millisecond(LTE_NETWORK_TIMEOUT)) {
        ret = lte_send_at("+CREG?", resp, sizeof(resp), LTE_CMD_TIMEOUT);
        if (ret == RT_EOK) {
            /* Parse +CREG: <n>,<stat> - stat=1(home) or stat=5(roaming) */
            char *p = strstr(resp, "+CREG:");
            if (p) {
                int n, stat;
                if (sscanf(p, "+CREG: %d,%d", &n, &stat) == 2) {
                    if (stat == 1 || stat == 5) {
                        s_status.network_ready = RT_TRUE;
                        s_status.state = LTE_STATE_NETWORK_REG;
                        LOG_I("  Network registered (stat=%d)", stat);
                        break;
                    }
                }
            }
        }
        rt_thread_mdelay(2000);
    }
    if (!s_status.network_ready) {
        LOG_E("Network registration timeout");
        s_status.state = LTE_STATE_ERROR;
        return -RT_ETIMEOUT;
    }

    /* Step 6: Auto-detect APN */
    LOG_I("[6/11] Detecting APN...");
    apn = detect_apn();

    /* Step 7: Attach to GPRS */
    LOG_I("[7/11] Attaching to GPRS...");
    ret = send_at_simple("+CGATT=1", LTE_CMD_TIMEOUT);
    if (ret != RT_EOK) {
        LOG_W("CGATT command failed, may already attached");
    }
    s_status.state = LTE_STATE_GPRS_ATTACHED;

    /* Step 8: Set APN */
    LOG_I("[8/11] Setting APN: %s...", apn);
    snprintf(s_tx_buf, TX_BUF_SIZE, "+CGDCONT=1,\"IP\",\"%s\"", apn);
    send_at_simple(s_tx_buf, LTE_CMD_TIMEOUT);

    /* Step 9: Activate PDP context */
    LOG_I("[9/11] Activating PDP context...");
    ret = send_at_simple("+CGACT=1,1", 5000);
    if (ret != RT_EOK) {
        LOG_W("PDP activation may already done");
    }

    /* Check if we got an IP address */
    ret = lte_send_at("+CGPADDR=1", resp, sizeof(resp), LTE_CMD_TIMEOUT);
    if (ret == RT_EOK) {
        LOG_I("  IP: %s", resp);
    }

    /* Step 10: Configure SSL and MQTT (Air780E EC618 platform) */
    /*
     * EMQX Cloud requires TLS/SSL on port 8883 (no plain TCP available)
     * Air780E needs SSL configuration before MQTT connection
     *
     * SSL Configuration:
     *   AT+CSSLCFG="sslversion",<ssl_ctx_index>,<sslversion>  (4=ALL)
     *   AT+CSSLCFG="authmode",<ssl_ctx_index>,<authmode>      (0=no verify)
     *   AT+CSSLCFG="ignorelocaltime",<ssl_ctx_index>,1        (ignore cert time)
     */
    LOG_I("[10/13] Configuring SSL...");
    s_status.state = LTE_STATE_MQTT_CONNECTING;

    /* Configure SSL context 0 for MQTT */
    send_at_simple("+CSSLCFG=\"sslversion\",0,4", LTE_CMD_TIMEOUT);      /* All TLS versions */
    send_at_simple("+CSSLCFG=\"authmode\",0,0", LTE_CMD_TIMEOUT);        /* No cert verification */
    send_at_simple("+CSSLCFG=\"ignorelocaltime\",0,1", LTE_CMD_TIMEOUT); /* Ignore cert time */
    send_at_simple("+CSSLCFG=\"enableSNI\",0,1", LTE_CMD_TIMEOUT);       /* Enable SNI */

    /* Clean up any existing MQTT connection */
    LOG_I("[11/13] Cleaning up old connections...");
    send_at_simple("+CMQTTDISC=0,60", 2000);
    send_at_simple("+CMQTTREL=0", 2000);
    send_at_simple("+CMQTTSTOP", 2000);
    rt_thread_mdelay(500);

    /*
     * Try multiple MQTT command sets (Air780E firmware varies):
     * Method 1: AT+CMQTT* (SIMCom compatible) with ssl:// URL
     * Method 2: AT+MQTT* native with SSLMIPSTART for SSL TCP
     * Method 3: AT+MQTT* native with MIPSTART (plain TCP, port 1883 only)
     */

    /* ========== Method 1: CMQTT commands (SIMCom style) ========== */
    LOG_I("[12/13] Trying MQTT connection (CMQTT method)...");

    ret = send_at_simple("+CMQTTSTART", 5000);
    if (ret == RT_EOK) {
        /* CMQTT supported, continue with this method */
        LOG_I("  CMQTT supported, configuring...");

        /* Acquire client: param3=1 means SSL server type */
        snprintf(s_tx_buf, TX_BUF_SIZE, "+CMQTTACCQ=0,\"%s\",1", s_config.client_id);
        ret = send_at_simple(s_tx_buf, LTE_CMD_TIMEOUT);
        if (ret != RT_EOK) {
            LOG_W("  CMQTTACCQ failed");
            goto try_method2;
        }

        /* Bind SSL context 0 to MQTT client 0 */
        send_at_simple("+CMQTTSSLCFG=0,0", LTE_CMD_TIMEOUT);

        /* Connect to broker - MUST use ssl:// for TLS port 8883 */
        LOG_I("  Connecting to %s:%d (SSL)...", s_config.broker, s_config.port);
        snprintf(s_tx_buf, TX_BUF_SIZE,
                 "+CMQTTCONNECT=0,\"ssl://%s:%d\",60,1,\"%s\",\"%s\"",
                 s_config.broker, s_config.port,
                 s_config.username, s_config.password);
        ret = lte_send_at(s_tx_buf, resp, sizeof(resp), LTE_MQTT_TIMEOUT);

        if (ret == RT_EOK && !strstr(resp, "ERROR")) {
            s_status.mqtt_connected = RT_TRUE;
            s_status.state = LTE_STATE_MQTT_CONNECTED;
            s_status.mqtt_method = 1;
            LOG_I("LTE MQTT connected! (CMQTT method, SSL)");
            return RT_EOK;
        }
        LOG_W("  CMQTT connect failed: %s", resp);

        /* Clean up before trying next method */
        send_at_simple("+CMQTTREL=0", 2000);
        send_at_simple("+CMQTTSTOP", 2000);
        rt_thread_mdelay(500);
    }

try_method2:
    /* ========== Method 2: Native MQTT with SSL TCP ========== */
    LOG_I("[13/13] Trying native MQTT method...");

    /* Stop CMQTT if it was started */
    send_at_simple("+CMQTTSTOP", 2000);
    rt_thread_mdelay(500);

    /* Configure MQTT parameters first */
    snprintf(s_tx_buf, TX_BUF_SIZE, "+MCONFIG=\"%s\",\"%s\",\"%s\"",
             s_config.client_id, s_config.username, s_config.password);
    ret = send_at_simple(s_tx_buf, LTE_CMD_TIMEOUT);
    if (ret != RT_EOK) {
        LOG_W("  MCONFIG not supported, trying MQTTCFG...");

        /* Some firmwares use MQTTCFG instead */
        snprintf(s_tx_buf, TX_BUF_SIZE,
                 "+MQTTCFG=\"%s\",%d,\"%s\",60,\"%s\",\"%s\"",
                 s_config.broker, s_config.port,
                 s_config.client_id, s_config.username, s_config.password);
        ret = send_at_simple(s_tx_buf, LTE_CMD_TIMEOUT);
    }

    /* Try SSL TCP connection first (SSLMIPSTART) */
    LOG_I("  Opening SSL TCP to %s:%d...", s_config.broker, s_config.port);
    snprintf(s_tx_buf, TX_BUF_SIZE, "+SSLMIPSTART=\"%s\",%d",
             s_config.broker, s_config.port);
    ret = lte_send_at(s_tx_buf, resp, sizeof(resp), LTE_MQTT_TIMEOUT);

    if (ret != RT_EOK || strstr(resp, "ERROR")) {
        LOG_W("  SSLMIPSTART failed (%s), trying MIPSTART with SSL flag...", resp);

        /* Some firmwares: AT+MIPSTART=<host>,<port>,1 (last param=SSL) */
        snprintf(s_tx_buf, TX_BUF_SIZE, "+MIPSTART=\"%s\",%d,1",
                 s_config.broker, s_config.port);
        ret = lte_send_at(s_tx_buf, resp, sizeof(resp), LTE_MQTT_TIMEOUT);
    }

    if (ret != RT_EOK || strstr(resp, "ERROR")) {
        LOG_E("All SSL TCP methods failed for port %d", s_config.port);
        LOG_E("Last response: %s", resp);
        LOG_E("Possible issues:");
        LOG_E("  1. AT firmware may not support MQTT SSL");
        LOG_E("  2. Try updating Air780E AT firmware");
        LOG_E("  3. Use a broker that supports plain TCP (port 1883)");
        s_status.state = LTE_STATE_ERROR;
        return -RT_ERROR;
    }

    /* SSL TCP connected, now send MQTT CONNECT */
    rt_thread_mdelay(1000);
    LOG_I("  SSL TCP connected, sending MQTT CONNECT...");

    ret = send_at_simple("+MCONNECT=1,120", LTE_MQTT_TIMEOUT);
    if (ret != RT_EOK) {
        LOG_E("MQTT CONNECT failed after SSL TCP OK");
        send_at_simple("+MIPCLOSE", 2000);
        s_status.state = LTE_STATE_ERROR;
        return -RT_ERROR;
    }

    s_status.mqtt_connected = RT_TRUE;
    s_status.state = LTE_STATE_MQTT_CONNECTED;
    s_status.mqtt_method = 2;
    LOG_I("LTE MQTT connected! (native method, SSL)");

    return RT_EOK;
}

/*---------------------------------------------------------------------------
 * Disconnect
 *---------------------------------------------------------------------------*/
void lte_disconnect(void)
{
    if (!s_initialized) return;

    /* Disconnect based on which method was used */
    if (s_status.mqtt_method == 1) {
        /* CMQTT method */
        send_at_simple("+CMQTTDISC=0,60", 2000);
        send_at_simple("+CMQTTREL=0", 2000);
        send_at_simple("+CMQTTSTOP", 2000);
    } else {
        /* Native method */
        send_at_simple("+MDISCONNECT", 2000);
        send_at_simple("+MIPCLOSE", 2000);
    }

    s_status.mqtt_connected = RT_FALSE;
    s_status.state = LTE_STATE_IDLE;
    LOG_I("LTE disconnected");
}

/*---------------------------------------------------------------------------
 * Check if Ready
 *---------------------------------------------------------------------------*/
rt_bool_t lte_is_ready(void)
{
    return s_status.mqtt_connected && (s_status.state == LTE_STATE_MQTT_CONNECTED);
}

/*---------------------------------------------------------------------------
 * Get Status
 *---------------------------------------------------------------------------*/
const lte_status_t* lte_get_status(void)
{
    return &s_status;
}

/*---------------------------------------------------------------------------
 * Publish to MQTT (with auto-reconnect trigger)
 * Supports two Air780E MQTT command sets:
 *   Method 1: AT+CMQTT* (SIMCom style)
 *   Method 2: AT+MPUB (native)
 *---------------------------------------------------------------------------*/
rt_err_t lte_mqtt_publish(const char *topic_suffix, const char *payload)
{
    if (!lte_is_ready()) {
        LOG_W("LTE not ready, triggering reconnect...");
        lte_start_reconnect();
        return -RT_ERROR;
    }

    char resp[128];
    rt_err_t ret;

    /* Build full topic using configured device_id */
    snprintf(s_topic_buf, TOPIC_BUF_SIZE, "guardian/%s/%s",
             s_config.device_id, topic_suffix);

    int topic_len = strlen(s_topic_buf);
    int payload_len = strlen(payload);

    if (s_status.mqtt_method == 1) {
        /* Method 1: CMQTT commands (SIMCom style) */
        /* Set topic first */
        snprintf(s_tx_buf, TX_BUF_SIZE, "+CMQTTTOPIC=0,%d", topic_len);
        ret = lte_send_at(s_tx_buf, resp, sizeof(resp), LTE_CMD_TIMEOUT);
        if (ret != RT_EOK && !strstr(resp, ">")) {
            LOG_E("Set topic failed");
            goto publish_failed;
        }
        rt_device_write(s_uart_dev, 0, s_topic_buf, topic_len);
        rt_thread_mdelay(100);

        /* Set payload */
        snprintf(s_tx_buf, TX_BUF_SIZE, "+CMQTTPAYLOAD=0,%d", payload_len);
        ret = lte_send_at(s_tx_buf, resp, sizeof(resp), LTE_CMD_TIMEOUT);
        if (ret != RT_EOK && !strstr(resp, ">")) {
            LOG_E("Set payload failed");
            goto publish_failed;
        }
        rt_device_write(s_uart_dev, 0, payload, payload_len);
        rt_thread_mdelay(100);

        /* Publish */
        ret = send_at_simple("+CMQTTPUB=0,0,60", LTE_CMD_TIMEOUT);
    } else {
        /* Method 2: Native MPUB command */
        if (payload_len < 200) {
            snprintf(s_tx_buf, TX_BUF_SIZE, "+MPUB=\"%s\",0,0,\"%s\"",
                     s_topic_buf, payload);
            ret = send_at_simple(s_tx_buf, LTE_CMD_TIMEOUT);
        } else {
            snprintf(s_tx_buf, TX_BUF_SIZE, "+MPUBEX=\"%s\",0,0,%d",
                     s_topic_buf, payload_len);
            ret = lte_send_at(s_tx_buf, resp, sizeof(resp), LTE_CMD_TIMEOUT);
            if (ret == RT_EOK || strstr(resp, ">")) {
                rt_device_write(s_uart_dev, 0, payload, payload_len);
                rt_thread_mdelay(100);
                rt_size_t rx_len = read_response(resp, sizeof(resp), LTE_CMD_TIMEOUT);
                ret = (rx_len > 0 && strstr(resp, "OK")) ? RT_EOK : -RT_ERROR;
            }
        }
    }

    if (ret == RT_EOK) {
        LOG_D("LTE -> %s: %s", s_topic_buf, payload);
        return RT_EOK;
    }

publish_failed:
    LOG_E("Publish failed, marking connection as lost");
    s_status.mqtt_connected = RT_FALSE;
    s_status.state = LTE_STATE_ERROR;
    lte_start_reconnect();
    return -RT_ERROR;
}

/*---------------------------------------------------------------------------
 * Get Signal Quality
 *---------------------------------------------------------------------------*/
int lte_get_signal_quality(void)
{
    char resp[64];
    rt_err_t ret = lte_send_at("+CSQ", resp, sizeof(resp), LTE_CMD_TIMEOUT);

    if (ret == RT_EOK) {
        /* Response: +CSQ: 18,0 */
        char *p = strstr(resp, "+CSQ:");
        if (p) {
            int csq = 0;
            sscanf(p, "+CSQ: %d", &csq);
            s_status.signal_quality = csq;
            return csq;
        }
    }

    return -1;
}

/*---------------------------------------------------------------------------
 * Background Reconnection Thread
 *---------------------------------------------------------------------------*/
static void lte_reconnect_thread_entry(void *parameter)
{
    rt_uint32_t attempt = 0;

    LOG_I("LTE reconnect thread started");

    while (attempt < LTE_MAX_RECONNECT_ATTEMPTS) {
        attempt++;
        LOG_I("LTE reconnect attempt %u/%u", attempt, LTE_MAX_RECONNECT_ATTEMPTS);

        /* Reset connection state */
        s_status.mqtt_connected = RT_FALSE;
        s_status.network_ready = RT_FALSE;
        s_status.state = LTE_STATE_INIT;

        /* Try to reconnect */
        if (lte_connect() == RT_EOK) {
            LOG_I("LTE reconnected successfully!");
            s_reconnect_requested = RT_FALSE;
            s_reconnect_thread = RT_NULL;
            return;
        }

        /* Wait before next attempt */
        LOG_W("Reconnect failed, waiting %d ms...", LTE_RECONNECT_INTERVAL_MS);
        rt_thread_mdelay(LTE_RECONNECT_INTERVAL_MS);
    }

    LOG_E("LTE reconnect failed after %u attempts", LTE_MAX_RECONNECT_ATTEMPTS);
    s_reconnect_requested = RT_FALSE;
    s_reconnect_thread = RT_NULL;
}

void lte_start_reconnect(void)
{
    /* Avoid multiple reconnect threads */
    if (s_reconnect_requested || s_reconnect_thread != RT_NULL) {
        LOG_D("Reconnect already in progress");
        return;
    }

    s_reconnect_requested = RT_TRUE;

    s_reconnect_thread = rt_thread_create("lte_reconn",
                                          lte_reconnect_thread_entry,
                                          RT_NULL,
                                          4096,
                                          20,
                                          10);
    if (s_reconnect_thread != RT_NULL) {
        rt_thread_startup(s_reconnect_thread);
        LOG_I("LTE reconnect thread started");
    } else {
        LOG_E("Failed to create reconnect thread");
        s_reconnect_requested = RT_FALSE;
    }
}

/*---------------------------------------------------------------------------
 * MSH Debug Commands
 *---------------------------------------------------------------------------*/
#ifdef RT_USING_FINSH
#include <finsh.h>

static void lte_test(int argc, char **argv)
{
    if (argc < 2) {
        rt_kprintf("Usage:\n");
        rt_kprintf("  lte_test init      - Initialize LTE\n");
        rt_kprintf("  lte_test connect   - Connect to network & MQTT\n");
        rt_kprintf("  lte_test disconnect- Disconnect MQTT\n");
        rt_kprintf("  lte_test status    - Show status\n");
        rt_kprintf("  lte_test signal    - Get signal quality\n");
        rt_kprintf("  lte_test pub       - Publish test message\n");
        rt_kprintf("  lte_test pub2 <msg>- Publish custom message\n");
        rt_kprintf("  lte_test fulltest  - Full verification test\n");
        rt_kprintf("  lte_test at <cmd>  - Send raw AT command\n");
        return;
    }

    if (strcmp(argv[1], "init") == 0) {
        rt_err_t ret = lte_init();
        rt_kprintf("LTE init: %s\n", ret == RT_EOK ? "OK" : "FAIL");
    }
    else if (strcmp(argv[1], "connect") == 0) {
        rt_kprintf("Starting LTE connection (may take 30-60 seconds)...\n");
        rt_err_t ret = lte_connect();
        rt_kprintf("LTE connect: %s\n", ret == RT_EOK ? "OK" : "FAIL");
    }
    else if (strcmp(argv[1], "disconnect") == 0) {
        lte_disconnect();
        rt_kprintf("LTE disconnected\n");
    }
    else if (strcmp(argv[1], "status") == 0) {
        const char *state_names[] = {
            "IDLE", "INIT", "SIM_READY", "NETWORK_REG",
            "GPRS_ATTACHED", "MQTT_CONNECTING", "MQTT_CONNECTED", "ERROR"
        };
        const lte_config_t *cfg = lte_get_config();
        rt_kprintf("\n===== LTE Status =====\n");
        rt_kprintf("  State:      %s\n", state_names[s_status.state]);
        rt_kprintf("  Signal:     %d/31\n", s_status.signal_quality);
        rt_kprintf("  Operator:   %s\n", s_status.operator_name[0] ? s_status.operator_name : "Unknown");
        rt_kprintf("  SIM:        %s\n", s_status.sim_ready ? "Ready" : "Not ready");
        rt_kprintf("  Network:    %s\n", s_status.network_ready ? "Registered" : "Not registered");
        rt_kprintf("  MQTT:       %s\n", s_status.mqtt_connected ? "Connected" : "Disconnected");
        rt_kprintf("  Reconnects: %u\n", s_status.reconnect_count);
        rt_kprintf("----- Config -----\n");
        rt_kprintf("  Broker:     %s:%d\n", cfg->broker, cfg->port);
        rt_kprintf("  Device ID:  %s\n", cfg->device_id);
        rt_kprintf("  Client ID:  %s\n", cfg->client_id);
        rt_kprintf("======================\n\n");
    }
    else if (strcmp(argv[1], "signal") == 0) {
        int csq = lte_get_signal_quality();
        rt_kprintf("Signal quality: %d", csq);
        if (csq == 99) rt_kprintf(" (unknown/no signal)");
        else if (csq < 10) rt_kprintf(" (weak)");
        else if (csq < 20) rt_kprintf(" (moderate)");
        else rt_kprintf(" (good)");
        rt_kprintf("\n");
    }
    else if (strcmp(argv[1], "pub") == 0) {
        rt_kprintf("Publishing test message...\n");
        rt_err_t ret = lte_mqtt_publish("test", "{\"source\":\"lte\",\"test\":true,\"value\":123}");
        rt_kprintf("Publish: %s\n", ret == RT_EOK ? "OK" : "FAIL");
    }
    else if (strcmp(argv[1], "pub2") == 0 && argc >= 3) {
        rt_kprintf("Publishing: %s\n", argv[2]);
        rt_err_t ret = lte_mqtt_publish("test", argv[2]);
        rt_kprintf("Publish: %s\n", ret == RT_EOK ? "OK" : "FAIL");
    }
    else if (strcmp(argv[1], "fulltest") == 0) {
        rt_kprintf("\n");
        rt_kprintf("========================================\n");
        rt_kprintf("  4G LTE Full Verification Test\n");
        rt_kprintf("========================================\n\n");

        /* Step 1: Init */
        rt_kprintf("[1/5] Initializing LTE module...\n");
        if (lte_init() != RT_EOK) {
            rt_kprintf("  FAIL: Cannot init LTE\n");
            return;
        }
        rt_kprintf("  OK\n\n");

        /* Step 2: Connect */
        rt_kprintf("[2/5] Connecting to network & MQTT...\n");
        rt_kprintf("  (This may take 30-60 seconds)\n");
        if (lte_connect() != RT_EOK) {
            rt_kprintf("  FAIL: Connection failed\n");
            return;
        }
        rt_kprintf("  OK - Connected!\n\n");

        /* Step 3: Check signal */
        rt_kprintf("[3/5] Checking signal quality...\n");
        int csq = lte_get_signal_quality();
        rt_kprintf("  Signal: %d/31 (%s)\n\n",
                   csq,
                   csq == 99 ? "no signal" :
                   csq < 10 ? "weak" :
                   csq < 20 ? "moderate" : "good");

        /* Step 4: Publish test messages */
        rt_kprintf("[4/5] Publishing test messages...\n");
        rt_err_t ret;

        rt_kprintf("  -> guardian/%s/test ... ", lte_get_config()->device_id);
        ret = lte_mqtt_publish("test", "{\"source\":\"4g\",\"step\":1}");
        rt_kprintf("%s\n", ret == RT_EOK ? "OK" : "FAIL");

        rt_thread_mdelay(1000);

        rt_kprintf("  -> guardian/%s/heartrate ... ", lte_get_config()->device_id);
        ret = lte_mqtt_publish("heartrate", "{\"heartrate\":75,\"source\":\"4g\"}");
        rt_kprintf("%s\n", ret == RT_EOK ? "OK" : "FAIL");

        rt_thread_mdelay(1000);

        rt_kprintf("  -> guardian/%s/env ... ", lte_get_config()->device_id);
        ret = lte_mqtt_publish("env", "{\"temp\":23.5,\"hum\":65,\"source\":\"4g\"}");
        rt_kprintf("%s\n", ret == RT_EOK ? "OK" : "FAIL");

        rt_kprintf("\n");

        /* Step 5: Summary */
        rt_kprintf("[5/5] Test Summary\n");
        rt_kprintf("  Operator:  %s\n", s_status.operator_name);
        rt_kprintf("  Signal:    %d/31\n", s_status.signal_quality);
        rt_kprintf("  MQTT:      %s\n", s_status.mqtt_connected ? "Connected" : "Disconnected");
        rt_kprintf("\n========================================\n");
        rt_kprintf("  4G LTE Test Complete!\n");
        rt_kprintf("  Check MQTTX or cloud dashboard for messages\n");
        rt_kprintf("========================================\n\n");
    }
    else if (strcmp(argv[1], "at") == 0 && argc >= 3) {
        char resp[256];
        rt_err_t ret = lte_send_at(argv[2], resp, sizeof(resp), 5000);
        rt_kprintf("Response (%s):\n%s\n", ret == RT_EOK ? "OK" : "FAIL", resp);
    }
}
MSH_CMD_EXPORT(lte_test, LTE module test commands);

#endif /* RT_USING_FINSH */

#endif /* #if 0 - ENTIRE FILE DISABLED */
