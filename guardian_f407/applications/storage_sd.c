/*
 * SD Card Storage Module
 * Target: STM32F407 + RT-Thread standard edition
 *
 * Hardware:
 *   SPI2 Bus:
 *   - PB13 -> SCK
 *   - PB14 -> MISO
 *   - PB15 -> MOSI
 *   - PB12 -> CS (directly controlled)
 *
 * Troubleshooting:
 *   1. If mount fails: check SPI wiring, card power (5V to module VCC)
 *   2. If read-only: check WP pin, or reformat card as FAT32 on PC
 *   3. If mkfs hangs: card may need FAT32 format from PC first
 *
 * Change Logs:
 * Date         Notes
 * 2026-03-21   first version
 * 2026-03-22   add diagnostic MSH commands
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_fs.h>
#include <dfs_posix.h>
#include <drv_common.h>
#include <drv_spi.h>

#define DBG_TAG "sdcard"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

#define SD_CS_PIN    GET_PIN(B, 12)  /* PB12 as SPI2 CS */

/* Declare external function from spi_msd driver */
extern rt_err_t msd_init(const char *sd_device_name, const char *spi_device_name);

/* Global status flags */
static rt_bool_t s_spi_attached = RT_FALSE;
static rt_bool_t s_msd_inited = RT_FALSE;
static rt_bool_t s_mounted = RT_FALSE;

int sdcard_mount(void)
{
    rt_err_t res;

    /* Wait for system to stabilize */
    rt_thread_mdelay(500);

    /* 1. Attach SPI device with CS pin */
    res = rt_hw_spi_device_attach("spi2", "spi20", GPIOB, GPIO_PIN_12);
    if (res != RT_EOK)
    {
        LOG_E("Failed to attach SPI device spi20, err: %d", res);
        return res;
    }
    s_spi_attached = RT_TRUE;
    LOG_I("SPI device 'spi20' attached to spi2");

    /* 2. Initialize SPI SD device */
    res = msd_init("sd0", "spi20");
    if (res != RT_EOK)
    {
        LOG_E("Failed to init SD device 'sd0', err: %d", res);
        LOG_E("Check: 1) SD card inserted? 2) Module VCC=5V? 3) Wiring correct?");
        return res;
    }
    s_msd_inited = RT_TRUE;
    LOG_I("SD device 'sd0' initialized");

    /* 3. Mount to root directory */
    res = dfs_mount("sd0", "/", "elm", 0, 0);
    if (res == RT_EOK)
    {
        s_mounted = RT_TRUE;
        LOG_I("SD card mounted to '/' successfully");
    }
    else
    {
        LOG_E("SD card mount failed! (err: %d)", res);
        LOG_W("Try: 1) Format card as FAT32 on PC  2) Run 'mkfs -t elm sd0' in MSH");
    }

    return res;
}
INIT_APP_EXPORT(sdcard_mount);

/*---------------------------------------------------------------------------
 * MSH Diagnostic Commands
 *---------------------------------------------------------------------------*/

/**
 * @brief Show SD card status
 */
static int cmd_sd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("\n===== SD Card Status =====\n");
    rt_kprintf("SPI attached:  %s\n", s_spi_attached ? "YES" : "NO");
    rt_kprintf("MSD init:      %s\n", s_msd_inited ? "YES" : "NO");
    rt_kprintf("FS mounted:    %s\n", s_mounted ? "YES" : "NO");

    /* Check if devices exist */
    rt_kprintf("\nDevice check:\n");
    rt_kprintf("  spi2:  %s\n", rt_device_find("spi2") ? "found" : "NOT FOUND");
    rt_kprintf("  spi20: %s\n", rt_device_find("spi20") ? "found" : "NOT FOUND");
    rt_kprintf("  sd0:   %s\n", rt_device_find("sd0") ? "found" : "NOT FOUND");

    /* Show mount info */
    if (s_mounted)
    {
        struct statfs buf;
        if (statfs("/", &buf) == 0)
        {
            rt_uint32_t total_kb = (buf.f_blocks * buf.f_bsize) / 1024;
            rt_uint32_t free_kb = (buf.f_bfree * buf.f_bsize) / 1024;
            rt_kprintf("\nFilesystem info:\n");
            rt_kprintf("  Total: %u KB\n", total_kb);
            rt_kprintf("  Free:  %u KB\n", free_kb);
            rt_kprintf("  Used:  %u KB\n", total_kb - free_kb);
        }
    }

    rt_kprintf("==========================\n\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_sd_status, sd_status, show SD card status);

/**
 * @brief Test SD card write capability
 */
static int cmd_sd_test(int argc, char **argv)
{
    int fd;
    int ret;
    const char *test_file = "/sd_test.txt";
    const char *test_data = "Guardian SD card test - RT-Thread\n";
    char read_buf[64] = {0};

    (void)argc;
    (void)argv;

    if (!s_mounted)
    {
        rt_kprintf("SD card not mounted! Run 'sd_status' to check.\n");
        return -1;
    }

    rt_kprintf("\n===== SD Card R/W Test =====\n");

    /* Test 1: Write */
    rt_kprintf("1. Writing test file...\n");
    fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        rt_kprintf("   FAILED to open file for writing! (err: %d)\n", fd);
        rt_kprintf("   Possible causes:\n");
        rt_kprintf("   - Card is write-protected (check WP switch)\n");
        rt_kprintf("   - Card needs FAT32 format from PC\n");
        rt_kprintf("   - File system corrupted\n");
        return -1;
    }

    ret = write(fd, test_data, rt_strlen(test_data));
    close(fd);

    if (ret != (int)rt_strlen(test_data))
    {
        rt_kprintf("   FAILED to write data! (wrote %d/%d bytes)\n", ret, rt_strlen(test_data));
        return -1;
    }
    rt_kprintf("   Write OK (%d bytes)\n", ret);

    /* Test 2: Read back */
    rt_kprintf("2. Reading test file...\n");
    fd = open(test_file, O_RDONLY);
    if (fd < 0)
    {
        rt_kprintf("   FAILED to open file for reading!\n");
        return -1;
    }

    ret = read(fd, read_buf, sizeof(read_buf) - 1);
    close(fd);

    if (ret <= 0)
    {
        rt_kprintf("   FAILED to read data!\n");
        return -1;
    }
    rt_kprintf("   Read OK (%d bytes)\n", ret);

    /* Test 3: Verify */
    rt_kprintf("3. Verifying data...\n");
    if (rt_strcmp(read_buf, test_data) == 0)
    {
        rt_kprintf("   Verify OK - Data matches!\n");
    }
    else
    {
        rt_kprintf("   Verify FAILED - Data mismatch!\n");
        rt_kprintf("   Expected: %s", test_data);
        rt_kprintf("   Got:      %s", read_buf);
        return -1;
    }

    /* Test 4: Delete */
    rt_kprintf("4. Deleting test file...\n");
    if (unlink(test_file) == 0)
    {
        rt_kprintf("   Delete OK\n");
    }
    else
    {
        rt_kprintf("   Delete FAILED (non-critical)\n");
    }

    rt_kprintf("\n*** SD Card Test PASSED! ***\n");
    rt_kprintf("============================\n\n");

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_sd_test, sd_test, test SD card read/write);

/**
 * @brief Remount SD card (useful after inserting card)
 */
static int cmd_sd_remount(int argc, char **argv)
{
    rt_err_t res;

    (void)argc;
    (void)argv;

    rt_kprintf("Remounting SD card...\n");

    /* Unmount first if mounted */
    if (s_mounted)
    {
        dfs_unmount("/");
        s_mounted = RT_FALSE;
        rt_kprintf("  Unmounted\n");
    }

    /* Try to mount again */
    res = dfs_mount("sd0", "/", "elm", 0, 0);
    if (res == RT_EOK)
    {
        s_mounted = RT_TRUE;
        rt_kprintf("  Mounted successfully!\n");
    }
    else
    {
        rt_kprintf("  Mount failed (err: %d)\n", res);
        rt_kprintf("  Try formatting: mkfs -t elm sd0\n");
    }

    return res;
}
MSH_CMD_EXPORT_ALIAS(cmd_sd_remount, sd_remount, remount SD card);

/**
 * @brief Low-level SD card write test (bypasses filesystem)
 * This writes directly to the block device to test if the SD card
 * hardware/driver supports writing.
 *
 * Note: Since the device is in STANDALONE mode and already opened by
 * the filesystem, we access it directly without open/close.
 */
static int cmd_sd_lowlevel(int argc, char **argv)
{
    rt_device_t dev;
    rt_uint8_t *buf;
    rt_uint8_t *buf_verify;
    rt_size_t ret;
    rt_uint32_t test_sector = 100;  /* Test on sector 100 (safe area, not boot sector) */

    (void)argc;
    (void)argv;

    rt_kprintf("\n===== SD Low-Level Test =====\n");
    rt_kprintf("Testing direct block device read/write\n");

    dev = rt_device_find("sd0");
    if (dev == RT_NULL)
    {
        rt_kprintf("ERROR: sd0 device not found!\n");
        return -1;
    }
    rt_kprintf("Device sd0 found, ref_count=%d\n", dev->ref_count);

    /* Allocate buffers */
    buf = rt_malloc(512);
    buf_verify = rt_malloc(512);
    if (buf == RT_NULL || buf_verify == RT_NULL)
    {
        rt_kprintf("ERROR: Memory allocation failed!\n");
        if (buf) rt_free(buf);
        if (buf_verify) rt_free(buf_verify);
        return -1;
    }

    /* Test 1: Read sector */
    rt_kprintf("\n1. Reading sector %u...\n", test_sector);
    ret = rt_device_read(dev, test_sector, buf, 1);
    if (ret != 1)
    {
        rt_kprintf("   READ FAILED! (ret=%d)\n", ret);
        rt_free(buf);
        rt_free(buf_verify);
        return -1;
    }
    rt_kprintf("   Read OK, first 16 bytes:\n   ");
    for (int i = 0; i < 16; i++)
        rt_kprintf("%02X ", buf[i]);
    rt_kprintf("\n");

    /* Test 2: Write sector (with same data to avoid corruption) */
    rt_kprintf("\n2. Writing sector %u (same data back)...\n", test_sector);
    ret = rt_device_write(dev, test_sector, buf, 1);
    if (ret != 1)
    {
        rt_kprintf("   WRITE FAILED! (ret=%d)\n", ret);
        rt_kprintf("\n   >>> SD card rejects write commands <<<\n");
        rt_kprintf("   Possible causes:\n");
        rt_kprintf("   1. Card has internal write protection\n");
        rt_kprintf("   2. Card is damaged or permanently read-only\n");
        rt_kprintf("   3. SPI signal quality issue\n");
        rt_kprintf("\n   Solutions to try:\n");
        rt_kprintf("   - Use a different SD card\n");
        rt_kprintf("   - Check module VCC is 5V (not 3.3V)\n");
        rt_kprintf("   - Check SPI wiring connections\n");
        rt_free(buf);
        rt_free(buf_verify);
        return -1;
    }
    rt_kprintf("   Write OK!\n");

    /* Test 3: Read back and verify */
    rt_kprintf("\n3. Verifying write by reading back...\n");
    rt_memset(buf_verify, 0xAA, 512);  /* Fill with pattern to detect read */
    ret = rt_device_read(dev, test_sector, buf_verify, 1);
    if (ret != 1)
    {
        rt_kprintf("   Verify READ FAILED!\n");
        rt_free(buf);
        rt_free(buf_verify);
        return -1;
    }

    /* Compare */
    if (rt_memcmp(buf, buf_verify, 512) == 0)
    {
        rt_kprintf("   Verify OK - Data matches!\n");
    }
    else
    {
        rt_kprintf("   Verify FAILED - Data mismatch!\n");
        rt_kprintf("   Original first 16: ");
        for (int i = 0; i < 16; i++)
            rt_kprintf("%02X ", buf[i]);
        rt_kprintf("\n   Readback first 16: ");
        for (int i = 0; i < 16; i++)
            rt_kprintf("%02X ", buf_verify[i]);
        rt_kprintf("\n");
        rt_free(buf);
        rt_free(buf_verify);
        return -1;
    }

    rt_kprintf("\n*** Low-Level Test PASSED! ***\n");
    rt_kprintf("SD card hardware supports read/write.\n");
    rt_kprintf("Problem is likely in filesystem layer.\n");
    rt_kprintf("=============================\n\n");

    rt_free(buf);
    rt_free(buf_verify);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_sd_lowlevel, sd_lowlevel, low-level SD read/write test);
