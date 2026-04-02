/*
 * GPS ATGM336H Driver — NMEA-0183 parser
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Supported sentences:
 *   $GPRMC  — time, lat, lon, speed, date, valid flag
 *   $GPGGA  — lat, lon, altitude, satellites, HDOP, fix type
 *
 * NMEA coordinate format:
 *   ddmm.mmmm  (degrees + minutes)
 *   Convert: degrees = dd + mm.mmmm / 60
 */

#define LOG_TAG     "gps"
#define LOG_LVL     DBG_INFO
#include <rtdbg.h>

#include "sensor_gps.h"
#include <string.h>
#include <stdlib.h>     /* atoi, atof */

/*---------------------------------------------------------------------------
 * Internal state
 *---------------------------------------------------------------------------*/
static rt_device_t      s_uart        = RT_NULL;
static gps_result_t     s_result;
static struct rt_mutex  s_mutex;
static rt_bool_t        s_initialized = RT_FALSE;

/* Line assembly buffer */
static char  s_line[GPS_LINE_BUF_SIZE];
static int   s_line_pos = 0;

/*---------------------------------------------------------------------------
 * NMEA helper functions
 *---------------------------------------------------------------------------*/

/**
 * Validate NMEA checksum.
 * Format: $<data>*<HH>   where HH = XOR of all bytes between $ and *
 */
static rt_bool_t _nmea_checksum_ok(const char *sentence)
{
    const char *p = sentence;
    rt_uint8_t  calc = 0;
    rt_uint8_t  recv;

    if (*p != '$')
        return RT_FALSE;
    p++;    /* skip $ */

    while (*p && *p != '*')
        calc ^= (rt_uint8_t)*p++;

    if (*p != '*')
        return RT_FALSE;
    p++;    /* skip * */

    /* parse two hex digits */
    recv = 0;
    if      (*p >= '0' && *p <= '9') recv  = (*p - '0') << 4;
    else if (*p >= 'A' && *p <= 'F') recv  = (*p - 'A' + 10) << 4;
    else if (*p >= 'a' && *p <= 'f') recv  = (*p - 'a' + 10) << 4;
    else return RT_FALSE;
    p++;
    if      (*p >= '0' && *p <= '9') recv |= (*p - '0');
    else if (*p >= 'A' && *p <= 'F') recv |= (*p - 'A' + 10);
    else if (*p >= 'a' && *p <= 'f') recv |= (*p - 'a' + 10);
    else return RT_FALSE;

    return (calc == recv) ? RT_TRUE : RT_FALSE;
}

/**
 * Get the n-th comma-separated field from an NMEA sentence into buf.
 * Field 0 = sentence type (e.g. "$GPRMC").
 * Returns RT_TRUE if field found and non-empty.
 */
static rt_bool_t _nmea_field(const char *sentence, int n, char *buf, int buf_len)
{
    const char *p = sentence;
    int field = 0;

    while (*p)
    {
        if (field == n)
        {
            int i = 0;
            while (*p && *p != ',' && *p != '*' && i < buf_len - 1)
                buf[i++] = *p++;
            buf[i] = '\0';
            return (i > 0) ? RT_TRUE : RT_FALSE;
        }
        if (*p == ',')
            field++;
        p++;
    }
    buf[0] = '\0';
    return RT_FALSE;
}

/**
 * Convert NMEA coordinate string (ddmm.mmmm or dddmm.mmmm) to decimal degrees.
 * dir: 'N'/'E' positive, 'S'/'W' negative.
 */
static float _nmea_to_degrees(const char *coord, char dir)
{
    float raw, deg, minutes;
    int   ideg;

    if (!coord || coord[0] == '\0')
        return 0.0f;

    raw     = (float)atof(coord);
    ideg    = (int)(raw / 100);
    minutes = raw - (float)(ideg * 100);
    deg     = (float)ideg + minutes / 60.0f;

    if (dir == 'S' || dir == 'W')
        deg = -deg;

    return deg;
}

/*---------------------------------------------------------------------------
 * NMEA sentence parsers
 *---------------------------------------------------------------------------*/

/**
 * Parse $GPRMC sentence.
 * Fields: type,time,status,lat,N/S,lon,E/W,speed,course,date,mag,mag_dir,mode*cs
 */
static void _parse_gprmc(const char *sentence)
{
    char f_time[12], f_status[4], f_lat[14], f_ns[4];
    char f_lon[14],  f_ew[4],    f_speed[10], f_date[10];

    _nmea_field(sentence, 1, f_time,   sizeof(f_time));
    _nmea_field(sentence, 2, f_status, sizeof(f_status));
    _nmea_field(sentence, 3, f_lat,    sizeof(f_lat));
    _nmea_field(sentence, 4, f_ns,     sizeof(f_ns));
    _nmea_field(sentence, 5, f_lon,    sizeof(f_lon));
    _nmea_field(sentence, 6, f_ew,     sizeof(f_ew));
    _nmea_field(sentence, 7, f_speed,  sizeof(f_speed));
    _nmea_field(sentence, 9, f_date,   sizeof(f_date));

    rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);

    /* Valid flag: 'A' = active (valid), 'V' = void */
    s_result.valid = (f_status[0] == 'A') ? 1 : 0;

    if (s_result.valid)
    {
        s_result.latitude    = _nmea_to_degrees(f_lat, f_ns[0]);
        s_result.longitude   = _nmea_to_degrees(f_lon, f_ew[0]);
        s_result.speed_knots = (float)atof(f_speed);

        /* Time: HHMMSS.ss */
        if (f_time[0])
        {
            s_result.hour   = (rt_uint8_t)((f_time[0]-'0')*10 + (f_time[1]-'0'));
            s_result.minute = (rt_uint8_t)((f_time[2]-'0')*10 + (f_time[3]-'0'));
            s_result.second = (rt_uint8_t)((f_time[4]-'0')*10 + (f_time[5]-'0'));
        }

        /* Date: DDMMYY */
        if (f_date[0])
        {
            s_result.day   = (rt_uint8_t)((f_date[0]-'0')*10 + (f_date[1]-'0'));
            s_result.month = (rt_uint8_t)((f_date[2]-'0')*10 + (f_date[3]-'0'));
            s_result.year  = (rt_uint16_t)(2000 + (f_date[4]-'0')*10 + (f_date[5]-'0'));
        }

        s_result.timestamp = rt_tick_get();
    }

    rt_mutex_release(&s_mutex);
}

/**
 * Parse $GPGGA sentence.
 * Fields: type,time,lat,N/S,lon,E/W,fix,sats,hdop,alt,M,geoid,M,,*cs
 */
static void _parse_gpgga(const char *sentence)
{
    char f_lat[14], f_ns[4], f_lon[14], f_ew[4];
    char f_fix[4],  f_sats[4], f_hdop[8], f_alt[12];

    _nmea_field(sentence, 2,  f_lat,  sizeof(f_lat));
    _nmea_field(sentence, 3,  f_ns,   sizeof(f_ns));
    _nmea_field(sentence, 4,  f_lon,  sizeof(f_lon));
    _nmea_field(sentence, 5,  f_ew,   sizeof(f_ew));
    _nmea_field(sentence, 6,  f_fix,  sizeof(f_fix));
    _nmea_field(sentence, 7,  f_sats, sizeof(f_sats));
    _nmea_field(sentence, 8,  f_hdop, sizeof(f_hdop));
    _nmea_field(sentence, 9,  f_alt,  sizeof(f_alt));

    rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);

    s_result.fix_type  = (rt_uint8_t)atoi(f_fix);
    s_result.satellites = (rt_uint8_t)atoi(f_sats);
    s_result.hdop      = (float)atof(f_hdop);

    if (s_result.fix_type > 0)
    {
        s_result.altitude  = (float)atof(f_alt);
        /* Also update lat/lon from GGA (more fields than RMC) */
        if (f_lat[0])
        {
            s_result.latitude  = _nmea_to_degrees(f_lat, f_ns[0]);
            s_result.longitude = _nmea_to_degrees(f_lon, f_ew[0]);
        }
        s_result.timestamp = rt_tick_get();
    }

    rt_mutex_release(&s_mutex);
}

/**
 * Dispatch a complete NMEA sentence to the correct parser.
 * Supports both $GP (GPS only) and $GN (GNSS combined) formats.
 */
static void _dispatch_sentence(const char *sentence)
{
    if (!_nmea_checksum_ok(sentence))
    {
        LOG_D("checksum fail: %s", sentence);
        return;
    }

    /* RMC sentence: $GPRMC or $GNRMC */
    if (rt_strncmp(sentence, "$GPRMC", 6) == 0 ||
        rt_strncmp(sentence, "$GNRMC", 6) == 0)
    {
        _parse_gprmc(sentence);
    }
    /* GGA sentence: $GPGGA or $GNGGA */
    else if (rt_strncmp(sentence, "$GPGGA", 6) == 0 ||
             rt_strncmp(sentence, "$GNGGA", 6) == 0)
    {
        _parse_gpgga(sentence);
    }
    /* Ignore all other sentence types */
}

/*---------------------------------------------------------------------------
 * GPS receive thread — reads bytes, assembles lines, dispatches
 *---------------------------------------------------------------------------*/
static void _gps_thread(void *param)
{
    char ch;
    (void)param;

    LOG_I("receive thread started");

    while (1)
    {
        /* Block until at least 1 byte available */
        if (rt_device_read(s_uart, 0, &ch, 1) != 1)
        {
            rt_thread_mdelay(10);
            continue;
        }

        if (ch == '$')
        {
            /* Start of new sentence — reset buffer */
            s_line_pos    = 0;
            s_line[0]     = '$';
            s_line_pos    = 1;
        }
        else if (ch == '\n')
        {
            /* End of sentence */
            if (s_line_pos > 0)
            {
                s_line[s_line_pos] = '\0';
                _dispatch_sentence(s_line);
                s_line_pos = 0;
            }
        }
        else if (ch != '\r')
        {
            /* Normal character — append if buffer has space */
            if (s_line_pos < GPS_LINE_BUF_SIZE - 1)
                s_line[s_line_pos++] = ch;
            else
            {
                /* Overflow — discard this line */
                s_line_pos = 0;
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * MSH command: gps
 *---------------------------------------------------------------------------*/
static int _cmd_gps(int argc, char **argv)
{
    gps_result_t r;
    (void)argc;
    (void)argv;

    gps_get_result(&r);

    if (!r.valid)
    {
        rt_kprintf("GPS: no fix  (sats=%u, fix_type=%u)\n",
                   r.satellites, r.fix_type);
        return 0;
    }

    /* Print latitude with sign */
    rt_int32_t lat_int  = (rt_int32_t)r.latitude;
    rt_uint32_t lat_frac = (rt_uint32_t)((r.latitude  < 0 ? -r.latitude  : r.latitude)  * 100000) % 100000;
    rt_int32_t lon_int  = (rt_int32_t)r.longitude;
    rt_uint32_t lon_frac = (rt_uint32_t)((r.longitude < 0 ? -r.longitude : r.longitude) * 100000) % 100000;

    rt_kprintf("GPS fix:\n");
    rt_kprintf("  Lat      : %d.%05u %c\n",
               lat_int < 0 ? -lat_int : lat_int, lat_frac,
               r.latitude  >= 0 ? 'N' : 'S');
    rt_kprintf("  Lon      : %d.%05u %c\n",
               lon_int < 0 ? -lon_int : lon_int, lon_frac,
               r.longitude >= 0 ? 'E' : 'W');
    rt_kprintf("  Altitude : %d m\n",   (rt_int32_t)r.altitude);
    rt_kprintf("  Speed    : %d knots\n", (rt_int32_t)r.speed_knots);
    rt_kprintf("  Sats     : %u  HDOP: %d.%01u\n",
               r.satellites,
               (rt_int32_t)r.hdop,
               (rt_uint32_t)(r.hdop * 10) % 10);
    rt_kprintf("  UTC time : %04u-%02u-%02u %02u:%02u:%02u\n",
               r.year, r.month, r.day,
               r.hour, r.minute, r.second);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_gps, gps, read GPS position fix);

/*---------------------------------------------------------------------------
 * MSH command: gps_raw — print raw NMEA sentences for 10 seconds
 *---------------------------------------------------------------------------*/
static int _cmd_gps_raw(int argc, char **argv)
{
    rt_device_t uart;
    char ch;
    rt_tick_t start;
    int duration = 10;  /* default 10 seconds */

    (void)argc;
    (void)argv;

    /* Allow custom duration: gps_raw [seconds] */
    if (argc >= 2)
        duration = atoi(argv[1]);
    if (duration <= 0 || duration > 60)
        duration = 10;

    /* Open UART3 directly for raw read */
    uart = rt_device_find(GPS_UART_NAME);
    if (uart == RT_NULL)
    {
        rt_kprintf("UART '%s' not found\n", GPS_UART_NAME);
        return -1;
    }

    rt_kprintf("=== GPS Raw Data (%d seconds) ===\n", duration);
    rt_kprintf("Listening on %s @ %d baud...\n\n", GPS_UART_NAME, GPS_BAUD_RATE);

    start = rt_tick_get();

    while (rt_tick_get() - start < duration * RT_TICK_PER_SECOND)
    {
        /* Non-blocking read */
        if (rt_device_read(uart, 0, &ch, 1) == 1)
        {
            /* Print character directly to console */
            if (ch == '\r')
                continue;   /* skip CR */
            rt_kprintf("%c", ch);
        }
        else
        {
            rt_thread_mdelay(1);
        }
    }

    rt_kprintf("\n=== End of GPS Raw Data ===\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(_cmd_gps_raw, gps_raw, print raw GPS NMEA data for N seconds);

/*---------------------------------------------------------------------------
 * Public API
 *---------------------------------------------------------------------------*/
rt_err_t gps_init(void)
{
    rt_thread_t       tid;
    rt_err_t          ret;
    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;

    if (s_initialized)
        return RT_EOK;

    /* Open UART3 */
    s_uart = rt_device_find(GPS_UART_NAME);
    if (s_uart == RT_NULL)
    {
        LOG_E("UART '%s' not found — check BSP_USING_UART3 in board.h", GPS_UART_NAME);
        return -RT_ENOSYS;
    }

    /* Configure baud rate */
    cfg.baud_rate = GPS_BAUD_RATE;
    cfg.data_bits = DATA_BITS_8;
    cfg.stop_bits = STOP_BITS_1;
    cfg.parity    = PARITY_NONE;
    rt_device_control(s_uart, RT_DEVICE_CTRL_CONFIG, &cfg);

    /* Open in blocking read mode */
    ret = rt_device_open(s_uart, RT_DEVICE_FLAG_INT_RX);
    if (ret != RT_EOK)
    {
        LOG_E("open '%s' failed: %d", GPS_UART_NAME, ret);
        return ret;
    }

    /* Initialise result mutex */
    ret = rt_mutex_init(&s_mutex, "gps_mtx", RT_IPC_FLAG_FIFO);
    if (ret != RT_EOK)
    {
        LOG_E("mutex init failed");
        rt_device_close(s_uart);
        return ret;
    }

    /* Create receive/parse thread */
    tid = rt_thread_create("gps",
                           _gps_thread,
                           RT_NULL,
                           768,
                           RT_THREAD_PRIORITY_MAX / 2 + 4,
                           20);
    if (tid == RT_NULL)
    {
        LOG_E("thread create failed");
        rt_mutex_detach(&s_mutex);
        rt_device_close(s_uart);
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);

    s_initialized = RT_TRUE;
    LOG_I("init OK — listening on %s @ %d baud", GPS_UART_NAME, GPS_BAUD_RATE);
    return RT_EOK;
}

rt_err_t gps_get_result(gps_result_t *result)
{
    RT_ASSERT(result != RT_NULL);

    if (!s_initialized)
    {
        rt_memset(result, 0, sizeof(*result));
        return RT_EOK;
    }

    rt_mutex_take(&s_mutex, RT_WAITING_FOREVER);
    *result = s_result;
    rt_mutex_release(&s_mutex);

    return RT_EOK;
}
