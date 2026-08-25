#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* SoftAP 配网的纯逻辑部分（主机可测，无 ESP 依赖）：
 * 会话状态机、凭据来源优先级、表单解析与校验、AP SSID 生成。 */

/* 凭据来源优先级：NVS 用户配置 > 编译期 wifi_config.h > 无。 */
typedef enum {
    POMO_WIFI_SRC_NONE = 0,
    POMO_WIFI_SRC_NVS,
    POMO_WIFI_SRC_BUILD,
} pomo_wifi_src_t;

/* 配网会话状态：IDLE -> (START) -> ACTIVE -> (SAVE) -> SAVED，
 * ACTIVE -> (TIMEOUT) -> IDLE；SAVED 停留至下次 START，
 * 由 UI 结合 wifi_state 呈现 CONNECTING/FAILED 反馈。 */
typedef enum {
    POMO_PROV_IDLE = 0,
    POMO_PROV_ACTIVE,
    POMO_PROV_SAVED,
} pomo_prov_state_t;

typedef enum {
    POMO_PROV_EV_START = 0,   /* 手动/首次启动配网会话 */
    POMO_PROV_EV_TIMEOUT,     /* 5 分钟无操作自动关闭 */
    POMO_PROV_EV_SAVE,        /* 用户提交有效凭据 */
} pomo_prov_ev_t;

pomo_prov_state_t pomo_prov_next(pomo_prov_state_t st, pomo_prov_ev_t ev);

/* 来源选择：NVS 优先，无 NVS 时回退编译期。 */
pomo_wifi_src_t pomo_wifi_src_pick(bool nvs_has, bool build_has);

/* 凭据校验（ESP WiFi 规范）：SSID 1..32 字节；
 * 密码为空 = 开放网络，否则 8..63 字节（WPA 最短 8）。 */
bool pomo_wifi_creds_valid(const char *ssid, const char *pass);

/* application/x-www-form-urlencoded 原地解码（%XX 与 '+' -> 空格）。
 * 非法序列或解码后长度 >= cap 时返回 -1，否则返回新长度。 */
int pomo_url_decode(char *buf, size_t cap);

/* 从表单体提取字段并解码到 out；字段缺失或超长返回 false。 */
bool pomo_form_field(const char *body, const char *name,
                     char *out, size_t cap);

/* 由 STA MAC 后两字节生成 AP SSID："WHIPLASH-A3F2"。 */
void pomo_ap_ssid_from_mac(const uint8_t mac[6], char out[16]);
