#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "wifi_prov_util.h"

/* SoftAP WiFi 配网：普通用户无需编译，浏览器烧录 merged.bin 后
 * 连接设备热点 WHIPLASH-XXXX，打开 http://192.168.4.1/ 填入
 * 2.4GHz 家用 WiFi 即可完成 SNTP 校时。
 *
 * 凭据保存在 ESP32 本机 NVS（namespace "whiplash_wifi"），
 * 不上传任何云端；固件永不打印/回显密码。 */

/* NVS 用户凭据（独立于番茄钟状态 blob，不共用版本号）。 */
bool pomo_wifi_user_creds_load(char *ssid, size_t ssid_cap,
                               char *pass, size_t pass_cap);
bool pomo_wifi_user_creds_save(const char *ssid, const char *pass);
void pomo_wifi_user_creds_clear(void);
bool pomo_wifi_user_creds_exist(void);

/* 异步启动一次配网会话（独立任务内拿 WiFi 驱动锁、开 AP + HTTP）。
 * 会话已在运行时返回 false。首次启动无任何凭据时会自动调用。 */
bool pomo_wifi_prov_start(void);

/* 供 pomodoro_time_init 调用：创建会话任务并缓存 AP SSID，幂等。 */
void pomo_wifi_prov_early_init(void);

pomo_prov_state_t pomo_wifi_prov_state(void);

/* 当前 AP SSID："WHIPLASH-XXXX"（MAC 后两字节）。无会话时为上次值。 */
const char *pomo_wifi_prov_ap_ssid(void);

/* 闲时屏结果横幅可见性：会话进行中，或保存后 15 分钟内。 */
bool pomo_wifi_prov_show_banner(void);
