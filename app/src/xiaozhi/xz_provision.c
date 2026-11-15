/*
 * Wi-Fi provisioning for the xiaozhi port on SF32LB57.
 *
 * Credentials live in the (otherwise unused, BT was removed) FAL partition
 * "ble" as one small record with a magic and a checksum.
 *
 * Captive page: GET /  -> form + SSID list (GET /scan)
 *               POST /save -> persisting the credentials and rebooting.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include <rtthread.h>

#include "bf0_hal.h"
#include "ulog.h"
#include "fal.h"

/* AIC8800 port-internal API: AP start + scan (route 1 has no rt_wlan). */
#include "wifi_port_priv.h"
#include "wifi_api.h"

#include "xiaozhi_client_public.h"
#include "xiaozhi_ui.h"
#include "xz_provision.h"

/* lwIP last: it defines read/write macros that clash with rtthread headers */
#include <sys/socket.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>

/* RT-Thread DHCP server (components/net/lwip-dhcpd). The AIC driver only
 * calls it when LWIP_DHCPS is defined, which this SDK does not do, so the
 * provisioning server starts it explicitly once the AP netif is up. */
extern void dhcpd_start(const char *netif_name);

#define TAG                 "xz_prov"
#define PROV_PART_NAME      "ble"
#define PROV_REC_MAGIC      0x58434647u   /* 'X','F','C','G' */

typedef struct {
    uint32_t magic;
    char     ssid[33];
    char     pwd[65];
    uint32_t sum;
} prov_rec_t;

static char g_ap_ssid[24];
static bool g_ap_up;

/* ------------------------------------------------------------- storage */
static uint32_t rec_sum(const prov_rec_t *r)
{
    uint32_t s = 0x5A5A5A5A;
    const uint8_t *p = (const uint8_t *)r->ssid;
    for (size_t i = 0; i < sizeof(r->ssid) + sizeof(r->pwd); i++)
        s = (s * 33) ^ p[i];
    return s;
}

static const struct fal_partition *prov_part(void)
{
    const struct fal_partition *part = fal_partition_find(PROV_PART_NAME);
    if (!part)
    {
        /* fall back to the other KVDB slot so provisioning still works */
        part = fal_partition_find("dfu");
    }
    return part;
}

bool xz_prov_load(char *ssid, size_t ssid_sz, char *pwd, size_t pwd_sz)
{
    const struct fal_partition *part = prov_part();
    prov_rec_t rec;

    if (ssid && ssid_sz) ssid[0] = 0;
    if (pwd && pwd_sz) pwd[0] = 0;
    if (!part)
        return false;

    if (fal_partition_read(part, 0, (uint8_t *)&rec, sizeof(rec)) != (int)sizeof(rec))
        return false;

    if (rec.magic != PROV_REC_MAGIC || rec.sum != rec_sum(&rec))
        return false;

    rec.ssid[sizeof(rec.ssid) - 1] = 0;
    rec.pwd[sizeof(rec.pwd) - 1] = 0;
    if (!rec.ssid[0])
        return false;

    if (ssid) rt_snprintf(ssid, ssid_sz, "%s", rec.ssid);
    if (pwd) rt_snprintf(pwd, pwd_sz, "%s", rec.pwd);
    return true;
}

bool xz_prov_save(const char *ssid, const char *pwd)
{
    const struct fal_partition *part = prov_part();
    prov_rec_t rec;

    if (!part || !ssid || !ssid[0])
        return false;

    rt_memset(&rec, 0, sizeof(rec));
    rec.magic = PROV_REC_MAGIC;
    rt_snprintf(rec.ssid, sizeof(rec.ssid), "%s", ssid);
    rt_snprintf(rec.pwd, sizeof(rec.pwd), "%s", pwd ? pwd : "");
    rec.sum = rec_sum(&rec);

    if (fal_partition_erase(part, 0, part->len) < 0)
        LOG_W(TAG, "erase failed");
    if (fal_partition_write(part, 0, (const uint8_t *)&rec, sizeof(rec)) != (int)sizeof(rec))
    {
        LOG_E(TAG, "write failed");
        return false;
    }
    LOG_I(TAG, "saved ssid='%s'", rec.ssid);
    return true;
}

void xz_prov_erase(void)
{
    const struct fal_partition *part = prov_part();
    if (part)
        fal_partition_erase(part, 0, part->len);
}

/* ----------------------------------------------------------- AP control */
const char *xz_prov_ap_ssid(void)
{
    return g_ap_ssid;
}

static void build_ap_ssid(void)
{
    const char *mac = xz_get_mac_address();
    char hex[16];
    size_t n = 0;

    /* The MAC string is "88:00:33:77:69:22"; the wpa_supplicant SET_NETWORK
     * command layer splits on ':', so a colon must not end up in the SSID. */
    for (const char *p = mac; p && *p && n < sizeof(hex) - 1; p++)
    {
        if (*p != ':')
            hex[n++] = *p;
    }
    hex[n] = 0;

    if (n >= 4)
        rt_snprintf(g_ap_ssid, sizeof(g_ap_ssid), XZ_PROV_AP_PREFIX "%c%c%c%c",
                    hex[n - 4], hex[n - 3], hex[n - 2], hex[n - 1]);
    else
        rt_snprintf(g_ap_ssid, sizeof(g_ap_ssid), XZ_PROV_AP_PREFIX "%04X",
                    (unsigned)(rt_tick_get() & 0xFFFF));
}

static int prov_ap_up(void)
{
    size_t len;

    if (g_ap_up)
        return 0;

    build_ap_ssid();

    len = strlen(g_ap_ssid);
    if (len > AIC_MAX_SSID_LEN - 1)
        len = AIC_MAX_SSID_LEN - 1;
    wifi_ap_cfg.aic_ap_ssid.length = (unsigned char)len;
    rt_memcpy(wifi_ap_cfg.aic_ap_ssid.array, g_ap_ssid, len);
    wifi_ap_cfg.aic_ap_ssid.array[len] = 0;

    len = strlen(XZ_PROV_AP_PASS);
    wifi_ap_cfg.aic_ap_passwd.length = (unsigned char)len;
    rt_memcpy(wifi_ap_cfg.aic_ap_passwd.array, XZ_PROV_AP_PASS, len);

    wifi_ap_cfg.band = 0;              /* 0 = 2.4 GHz */
    wifi_ap_cfg.type = 0;              /* 20 MHz */
    wifi_ap_cfg.channel = 6;
    wifi_ap_cfg.hidden_ssid = 0;
    /* Plain WPA2-PSK on a fixed channel: the simplest configuration for this
     * firmware. Note the SET_NETWORK ssid timeout is not caused by these
     * settings - the FW asserts on HAL_AC3_TIMER_BIT and resets the link while
     * hostapd builds the BSS, so the hostapd control reply is lost. */
    wifi_ap_cfg.enable_acs = 0;
    wifi_ap_cfg.enable_he = 0;
    wifi_ap_cfg.sercurity_type = KEY_WPA2;

    /* The BSS setup runs into an FW timer assert (HAL_AC3_TIMER_BIT) right
     * after hostapd configures WPS, which resets the link and swallows the
     * reply to SET_NETWORK ssid. A provisioning AP does not need WPS. */
    app_wlan_wps_disable();

    if (app_wlan_bss_config(1) < 0)
    {
        LOG_E(TAG, "SoftAP start failed");
        return -1;
    }

    g_ap_up = true;
    LOG_I(TAG, "SoftAP '%s' up, page %s", g_ap_ssid, XZ_PROV_URL);
    return 0;
}

/* Give the driver a moment to bring the AP netif up, then hand out leases. */
static void dhcp_thread(void *arg)
{
    struct netif *n;

    (void)arg;
    rt_thread_mdelay(1500);
    for (n = netif_list; n; n = n->next)
    {
        if (n->name[0] == 'w' && n->name[1] == 'l')
        {
            LOG_I(TAG, "dhcpd_start(%s)", n->name);
            dhcpd_start(n->name);
            return;
        }
    }
    LOG_W(TAG, "no wl netif found for dhcpd");
}

/* ------------------------------------------------------------ HTTP page */
static const char page_html[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>小智 Wi-Fi 配网</title><style>"
    "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#111;color:#eee;"
    "margin:0;padding:24px}div{max-width:420px;margin:0 auto}h1{font-size:20px}"
    "input,select,button{width:100%;padding:12px;margin:8px 0;border-radius:10px;border:1px solid #444;"
    "background:#1c1c1c;color:#eee;font-size:16px;box-sizing:border-box}"
    "button{background:#ffc94a;color:#111;font-weight:600;border:0}"
    "small{color:#888}</style></head><body><div>"
    "<h1>小智 Wi-Fi 配网</h1>"
    "<p><small>选择或输入 2.4G Wi-Fi 名称与密码</small></p>"
    "<select id='list'><option value=''>-- 扫描附近 Wi-Fi --</option></select>"
    "<input id='ssid' placeholder='Wi-Fi 名称 (SSID)' maxlength='32'>"
    "<input id='pwd' type='password' placeholder='Wi-Fi 密码' maxlength='64'>"
    "<button onclick='save()'>连接</button>"
    "<p id='msg'><small></small></p></div>"
    "<script>"
    "fetch('/scan').then(r=>r.json()).then(a=>{let s=document.getElementById('list');"
    "a.forEach(x=>{let o=document.createElement('option');o.value=x;o.text=x;s.add(o)})})"
    ".catch(()=>{});"
    "document.getElementById('list').onchange=e=>document.getElementById('ssid').value=e.target.value;"
    "function save(){let s=document.getElementById('ssid').value.trim();"
    "let p=document.getElementById('pwd').value;"
    "if(!s){document.getElementById('msg').innerText='请输入 Wi-Fi 名称';return}"
    "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'ssid='+encodeURIComponent(s)+'&password='+encodeURIComponent(p)})"
    ".then(r=>r.text()).then(t=>{document.getElementById('msg').innerText=t})"
    ".catch(e=>{document.getElementById('msg').innerText='保存失败: '+e})}"
    "</script></body></html>";

static int send_all(int s, const char *buf, int len)
{
    int sent = 0;
    while (sent < len)
    {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0)
            return -1;
        sent += n;
    }
    return sent;
}

static void http_reply(int c, int code, const char *ctype, const char *body)
{
    char hdr[192];
    int body_len = (int)strlen(body);

    rt_snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                code, code == 200 ? "OK" : "Bad Request", ctype, body_len);
    send_all(c, hdr, (int)strlen(hdr));
    send_all(c, body, body_len);
}

static int url_decode(const char *in, char *out, int out_sz)
{
    int o = 0;
    while (*in && o < out_sz - 1)
    {
        char ch = *in++;
        if (ch == '+')
            ch = ' ';
        else if (ch == '%' && in[0] && in[1])
        {
            char hex[3] = {in[0], in[1], 0};
            ch = (char)strtol(hex, NULL, 16);
            in += 2;
        }
        out[o++] = ch;
    }
    out[o] = 0;
    return o;
}

static int form_value(const char *body, const char *key, char *out, int out_sz)
{
    char pat[40];
    const char *p;

    out[0] = 0;
    rt_snprintf(pat, sizeof(pat), "%s=", key);
    p = strstr(body, pat);
    if (!p)
        return 0;
    p += strlen(pat);

    char raw[128];
    int i = 0;
    while (p[i] && p[i] != '&' && i < (int)sizeof(raw) - 1)
    {
        raw[i] = p[i];
        i++;
    }
    raw[i] = 0;
    return url_decode(raw, out, out_sz);
}

static void http_scan_json(int c)
{
    static wifi_ap_list_t list;
    char body[1400];
    int n = 0;

    rt_memset(&list, 0, sizeof(list));
    aic_wifi_getscan(&list);

    n += rt_snprintf(body + n, sizeof(body) - n, "[");
    for (int i = 0; i < (int)list.ap_count && i < (int)MAX_AP_COUNT && n < (int)sizeof(body) - 80; i++)
    {
        const char *ssid = (const char *)list.ap_info[i].ssid;
        if (!ssid[0])
            continue;
        n += rt_snprintf(body + n, sizeof(body) - n, "%s\"%s\"",
                         (n > 1) ? "," : "", ssid);
    }
    n += rt_snprintf(body + n, sizeof(body) - n, "]");
    http_reply(c, 200, "application/json", body);
}

static void http_handle(int c)
{
    char req[768];
    int got = 0, n;

    rt_memset(req, 0, sizeof(req));
    while (got < (int)sizeof(req) - 1)
    {
        n = recv(c, req + got, sizeof(req) - 1 - got, 0);
        if (n <= 0)
            break;
        got += n;
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    if (got <= 0)
        return;

    if (!strncmp(req, "GET /scan", 9))
    {
        http_scan_json(c);
        return;
    }

    if (!strncmp(req, "POST /save", 10))
    {
        const char *body = strstr(req, "\r\n\r\n");
        char ssid[40], pwd[80];

        body = body ? body + 4 : "";
        form_value(body, "ssid", ssid, sizeof(ssid));
        form_value(body, "password", pwd, sizeof(pwd));

        if (ssid[0] && xz_prov_save(ssid, pwd))
        {
            http_reply(c, 200, "text/plain; charset=utf-8",
                       "已保存，设备正在重启并连接 Wi-Fi…");
            rt_thread_mdelay(1200);
            HAL_NVIC_SystemReset();
            return;
        }
        http_reply(c, 400, "text/plain; charset=utf-8", "保存失败，请重试");
        return;
    }

    http_reply(c, 200, "text/html; charset=utf-8", page_html);
}

static void http_thread(void *arg)
{
    int srv, on = 1;
    struct sockaddr_in addr;

    (void)arg;
    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
    {
        LOG_E(TAG, "socket failed");
        return;
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    rt_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 4) < 0)
    {
        LOG_E(TAG, "bind/listen 80 failed");
        closesocket(srv);
        return;
    }
    LOG_I(TAG, "config page listening on :80");

    for (;;)
    {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = accept(srv, (struct sockaddr *)&peer, &plen);
        if (c < 0)
            continue;

        struct timeval tv = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        http_handle(c);
        closesocket(c);
    }
}

/* Captive-portal helper: answer every A query with 192.168.4.1. */
static void dns_thread(void *arg)
{
    int s;
    struct sockaddr_in addr;

    (void)arg;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;

    rt_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        closesocket(s);
        return;
    }

    for (;;)
    {
        uint8_t buf[256];
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &plen);

        if (n < 12)
            continue;

        buf[2] = 0x81;   /* response, recursion available */
        buf[3] = 0x80;
        buf[6] = 0;      /* answer count = 1 */
        buf[7] = 1;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        n = 12;
        buf[n++] = 0xC0; buf[n++] = 0x0C;          /* name pointer */
        buf[n++] = 0x00; buf[n++] = 0x01;          /* A */
        buf[n++] = 0x00; buf[n++] = 0x01;          /* IN */
        buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x00; buf[n++] = 0x3C;
        buf[n++] = 0x00; buf[n++] = 0x04;
        buf[n++] = 192; buf[n++] = 168; buf[n++] = 4; buf[n++] = 1;

        sendto(s, buf, n, 0, (struct sockaddr *)&peer, plen);
    }
}

/* ----------------------------------------------------------------- entry */
void xz_prov_start(void)
{
    if (prov_ap_up() < 0)
    {
        xiaozhi_ui_chat_status("Wi-Fi AP failed");
        /* The AIC8800 SoftAP path is unreliable on this firmware (see
         * prov_ap_up); the console path always works. */
        LOG_E(TAG, "SoftAP unavailable, provision over the console: xz_wifi_join <ssid> [password]");
        return;
    }

    xiaozhi_ui_config_mode(xz_prov_ap_ssid(), XZ_PROV_AP_PASS);

    rt_thread_t t = rt_thread_create("prov_dhcp", dhcp_thread, NULL, 1536, 14, 10);
    if (t)
        rt_thread_startup(t);

    t = rt_thread_create("prov_http", http_thread, NULL, 3072, 12, 10);
    if (t)
        rt_thread_startup(t);

    t = rt_thread_create("prov_dns", dns_thread, NULL, 1536, 13, 10);
    if (t)
        rt_thread_startup(t);
}
