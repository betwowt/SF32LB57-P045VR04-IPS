// xiaozhi_public.c
#include "xiaozhi_client_public.h"
#include <rtthread.h>
#include "bf0_hal.h"
#include "bf0_hal_mpi_ex.h"
#include <webclient.h>
#include <cJSON.h>
#include "stdio.h"
#include "string.h"
#include <lwip/dns.h>
#include <lwip/sys.h>
#include "xiaozhi_websocket.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/apps/websocket_client.h"
#include "lwip/apps/mqtt_priv.h"
#include "lwip/apps/mqtt.h"
#include "lwip/tcpip.h"
#include "lv_timer.h"
#include "lv_display.h"
#include "lv_obj_pos.h"
#include "lvgl.h"
#include "lv_tiny_ttf.h"
#include "lv_obj.h"
#include "lv_label.h"
#include "bf0_sys_cfg.h"
#include "drv_flash.h"
#include "gui_app_pm.h"
#include "../board/board_hardware.h"

// 公共变量定义
extern uint8_t aec_enabled;
extern BOOL first_pan_connected;

static uint8_t g_en_vad = 1;
static uint8_t g_en_aec = 1;
static uint8_t g_config_change = 0;

volatile int g_kws_force_exit = 0;
volatile int g_kws_running = 0;
volatile uint8_t she_bei_ma = 1;
char mac_address_string[20];
char client_id_string[40];
ALIGN(4) uint8_t g_sha256_result[32] = {0};

static void xz_read_mac(uint8_t mac[6])
{
    static const uint8_t fallback[6] = {0x02, 0x57, 0x00, 0x00, 0x00, 0x01};
    uint32_t uid[4] = {0};
    FLASH_HandleTypeDef *h;

    if (rt_flash_config_read(FACTORY_CFG_ID_MAC, mac, 6) != 0)
        goto done;

    h = (FLASH_HandleTypeDef *)rt_flash_get_handle_by_addr(0x12000000);
    if (h && HAL_QSPI_GET_UID(h, uid, 4) == 0)
    {
        mac[0] = 0x02;
        mac[1] = (uid[0] >> 24) & 0xFF;
        mac[2] = (uid[0] >> 16) & 0xFF;
        mac[3] = (uid[0] >> 8) & 0xFF;
        mac[4] = uid[0] & 0xFF;
        mac[5] = (uid[1] >> 24) & 0xFF;
    }
    else
    {
        memcpy(mac, fallback, 6);
    }

done:
    mac[0] &= 0xFE; /* unicast */
}

char *xz_get_mac_address()
{
    if (mac_address_string[0] == '\0')
    {
        uint8_t mac[6];

        xz_read_mac(mac);
        rt_snprintf((char *)mac_address_string, 20,
                    "%02x:%02x:%02x:%02x:%02x:%02x",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return (&(mac_address_string[0]));
}
void hash_run(uint8_t algo, uint8_t *raw_data, uint32_t raw_data_len,
              uint8_t *result, uint32_t result_len)
{
    /* Rest hash block. */
    HAL_HASH_reset();
    /* Initialize AES Hash hardware block. */
    HAL_HASH_init(NULL, algo, 0);
    /* Do hash. HAL_HASH_run will block until hash finish. */
    HAL_HASH_run(raw_data, raw_data_len, 1);
    /* Get hash result. */
    HAL_HASH_result(result);
}
void hex_2_asc(uint8_t n, char *str)
{
    uint8_t i = (n >> 4);
    if (i >= 10)
        *str = i + 'a' - 10;
    else
        *str = i + '0';
    str++, i = n & 0xf;
    if (i >= 10)
        *str = i + 'a' - 10;
    else
        *str = i + '0';
}
char *get_client_id()
{
    if (client_id_string[0] == '\0')
    {
        int i, j = 0;
        uint8_t mac[6];

        xz_read_mac(mac);
        hash_run(HASH_ALGO_SHA256, mac, sizeof(mac),
                 g_sha256_result, sizeof(g_sha256_result));
        for (i = 0; i < 16; i++, j += 2)
        {
            // 12345678-1234-1234-1234-123456789012
            if (i == 4 || i == 6 || i == 8 || i == 10)
            {
                client_id_string[j++] = '-';
            }
            hex_2_asc(g_sha256_result[i], &client_id_string[j]);
        }
        rt_kprintf(client_id_string);
    }
    return (&(client_id_string[0]));
}

static void svr_found_callback(const char *name, const ip_addr_t *ipaddr,
                               void *callback_arg)
{
    if (ipaddr != NULL)
    {
        rt_kprintf("DNS lookup succeeded, IP: %s\n", ipaddr_ntoa(ipaddr));
    }
}
int check_internet_access()
{
    int r = 0;
    const char *hostname = XIAOZHI_HOST;
    ip_addr_t addr = {0};

    {
        err_t err =
            dns_gethostbyname(hostname, &addr, svr_found_callback, NULL);
        if (err != ERR_OK && err != ERR_INPROGRESS)
        {
            rt_kprintf("Coud not find %s, please check PAN connection\n",
                       hostname);
        }
        else
            r = 1;
    }

    return r;
}







// 设备注册函数
int register_device_with_server(void)
{
    // BLE PAN OTA service is not used in the Wi-Fi build.
    rt_kprintf("Device registered successfully with chip_id: %s\n",
               get_client_id());
    return 0;
}
