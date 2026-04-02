/*
 * Air780E 4G LTE Module Driver
 *
 * !! DISABLED !!
 * Reason: Air780E AT firmware does not support MQTT over TLS.
 *   - Module's MQTT command set: MCONFIG / MIPSTART / MCONNECT (plain TCP only)
 *   - EMQX Cloud free tier: port 8883 (TLS mandatory), port 1883 not available
 *   - AT+CMQTTSTART returns "command not found" (no CMQTT command set in firmware)
 *   - MIPSTART to port 8883 returns +CME ERROR: 767 (TCP rejected by TLS port)
 * To re-enable: flash firmware with CMQTT support OR use a broker with port 1883.
 *
 * Hardware: Air780E (M100p with AT firmware)
 * Interface: UART4 (PA0=TX, PA1=RX)
 * Baud rate: 115200
 */

#ifndef __LTE_DRIVER_H__
#define __LTE_DRIVER_H__

/* All declarations disabled - see file header for reason */
#if 0

#ifdef __cplusplus
extern "C" {
#endif

/*---------------------------------------------------------------------------
 * Configuration
 *---------------------------------------------------------------------------*/
#define LTE_UART_NAME           "uart4"
#define LTE_UART_BAUDRATE       115200

/* MQTT Configuration - configurable via lte_set_config() or use defaults */
/* Default values (can be overridden at runtime) */
#define LTE_MQTT_BROKER_DEFAULT     "b1117f90.ala.cn-hangzhou.emqxsl.cn"
#define LTE_MQTT_PORT_DEFAULT       8883
#define LTE_MQTT_USERNAME_DEFAULT   "user1"
#define LTE_MQTT_PASSWORD_DEFAULT   "12345"
#define LTE_MQTT_CLIENT_ID_DEFAULT  "air780e-001"
#define LTE_MQTT_DEVICE_ID_DEFAULT  "001"

/* APN Configuration - auto-detect or configure */
#define LTE_APN_CMNET           "CMNET"     /* China Mobile */
#define LTE_APN_UNINET          "3GNET"     /* China Unicom */
#define LTE_APN_CTNET           "CTNET"     /* China Telecom */

/* Timeouts (ms) */
#define LTE_CMD_TIMEOUT         3000
#define LTE_MQTT_TIMEOUT        15000       /* Increased from 10000 */
#define LTE_NETWORK_TIMEOUT     60000       /* Increased from 30000 */

/* Reconnection settings */
#define LTE_MAX_RECONNECT_ATTEMPTS  5
#define LTE_RECONNECT_INTERVAL_MS   10000

/*---------------------------------------------------------------------------
 * Status Definitions
 *---------------------------------------------------------------------------*/
typedef enum {
    LTE_STATE_IDLE = 0,         /* Not initialized */
    LTE_STATE_INIT,             /* Initializing module */
    LTE_STATE_SIM_READY,        /* SIM card ready */
    LTE_STATE_NETWORK_REG,      /* Registered to network */
    LTE_STATE_GPRS_ATTACHED,    /* GPRS attached */
    LTE_STATE_MQTT_CONNECTING,  /* Connecting to MQTT */
    LTE_STATE_MQTT_CONNECTED,   /* MQTT connected, ready to publish */
    LTE_STATE_ERROR,            /* Error state */
} lte_state_t;

typedef struct {
    lte_state_t state;          /* Current state */
    int signal_quality;         /* Signal strength (0-31, 99=unknown) */
    rt_bool_t sim_ready;        /* SIM card detected */
    rt_bool_t network_ready;    /* Network registered */
    rt_bool_t mqtt_connected;   /* MQTT connection status */
    uint32_t last_error;        /* Last error code */
    uint32_t reconnect_count;   /* Reconnection attempts */
    char operator_name[16];     /* Network operator name */
    uint8_t mqtt_method;        /* Which MQTT command set worked (1 or 2) */
} lte_status_t;

/* Runtime configuration structure */
typedef struct {
    char broker[64];            /* MQTT broker address */
    uint16_t port;              /* MQTT port */
    char username[32];          /* MQTT username */
    char password[32];          /* MQTT password */
    char client_id[32];         /* MQTT client ID */
    char device_id[16];         /* Device ID for topics */
    char apn[16];               /* APN (auto-detect if empty) */
} lte_config_t;

/*---------------------------------------------------------------------------
 * Public Functions
 *---------------------------------------------------------------------------*/

/**
 * Initialize the LTE module
 * @return RT_EOK on success
 */
rt_err_t lte_init(void);

/**
 * Set runtime configuration (call before lte_connect)
 * @param config Pointer to configuration structure (NULL to use defaults)
 */
void lte_set_config(const lte_config_t *config);

/**
 * Get current configuration
 * @return Pointer to current configuration
 */
const lte_config_t* lte_get_config(void);

/**
 * Start LTE connection (blocking, may take 60+ seconds)
 * @return RT_EOK on success
 */
rt_err_t lte_connect(void);

/**
 * Disconnect and power down the module
 */
void lte_disconnect(void);

/**
 * Check if LTE is connected and ready to send
 * @return RT_TRUE if ready
 */
rt_bool_t lte_is_ready(void);

/**
 * Get current LTE status
 * @return Pointer to status structure
 */
const lte_status_t* lte_get_status(void);

/**
 * Publish data to MQTT topic
 * @param topic_suffix Topic suffix (e.g., "heartrate", "env", "alert/fall")
 * @param payload JSON payload string
 * @return RT_EOK on success
 */
rt_err_t lte_mqtt_publish(const char *topic_suffix, const char *payload);

/**
 * Send raw AT command and get response
 * @param cmd AT command (without AT prefix)
 * @param resp Response buffer
 * @param resp_size Response buffer size
 * @param timeout Timeout in ms
 * @return RT_EOK on success
 */
rt_err_t lte_send_at(const char *cmd, char *resp, rt_size_t resp_size, rt_uint32_t timeout);

/**
 * Get signal quality (CSQ)
 * @return Signal strength (0-31), -1 on error
 */
int lte_get_signal_quality(void);

/**
 * Start background reconnection (non-blocking)
 * Creates a thread to handle reconnection
 */
void lte_start_reconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* #if 0 - DISABLED */

#endif /* __LTE_DRIVER_H__ */
