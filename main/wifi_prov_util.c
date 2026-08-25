#include "wifi_prov_util.h"

#include <string.h>

pomo_prov_state_t pomo_prov_next(pomo_prov_state_t st, pomo_prov_ev_t ev) {
    switch (st) {
        case POMO_PROV_IDLE:
            return ev == POMO_PROV_EV_START ? POMO_PROV_ACTIVE : POMO_PROV_IDLE;
        case POMO_PROV_ACTIVE:
            if (ev == POMO_PROV_EV_SAVE) return POMO_PROV_SAVED;
            if (ev == POMO_PROV_EV_TIMEOUT) return POMO_PROV_IDLE;
            return POMO_PROV_ACTIVE;
        case POMO_PROV_SAVED:
            return ev == POMO_PROV_EV_START ? POMO_PROV_ACTIVE : POMO_PROV_SAVED;
    }
    return POMO_PROV_IDLE;
}

pomo_wifi_src_t pomo_wifi_src_pick(bool nvs_has, bool build_has) {
    if (nvs_has) return POMO_WIFI_SRC_NVS;
    if (build_has) return POMO_WIFI_SRC_BUILD;
    return POMO_WIFI_SRC_NONE;
}

bool pomo_wifi_creds_valid(const char *ssid, const char *pass) {
    if (!ssid || !pass) return false;
    size_t sl = strlen(ssid);
    size_t pl = strlen(pass);
    if (sl < 1 || sl > 32) return false;
    if (pl == 0) return true;             /* 开放网络 */
    if (pl < 8 || pl > 63) return false;  /* WPA 密码 8..63 */
    return true;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* 解码 src[0..len) 到 out（可原地：写指针恒不超前读指针）。
 * 非法序列或解码后长度装不下（>= cap）返回 -1。 */
static int url_decode_range(const char *src, size_t len, char *out, size_t cap) {
    size_t r = 0, w = 0;
    while (r < len) {
        char c = src[r];
        if (c == '%') {
            if (r + 2 >= len) return -1;
            int hi = hex_val(src[r + 1]);
            int lo = hex_val(src[r + 2]);
            if (hi < 0 || lo < 0) return -1;
            out[w++] = (char)((hi << 4) | lo);
            r += 3;
        } else if (c == '+') {
            out[w++] = ' ';
            r += 1;
        } else {
            out[w++] = c;
            r += 1;
        }
        if (w >= cap) return -1;
    }
    out[w] = '\0';
    return (int)w;
}

int pomo_url_decode(char *buf, size_t cap) {
    if (!buf) return -1;
    return url_decode_range(buf, strlen(buf), buf, cap);
}

bool pomo_form_field(const char *body, const char *name,
                     char *out, size_t cap) {
    if (!body || !name || !out || cap == 0) return false;
    size_t nlen = strlen(name);
    const char *p = body;

    while (p) {
        const char *eq = strchr(p, '=');
        const char *sep = strchr(p, '&');
        if (!eq || (sep && eq > sep)) {
            /* 无值字段（"key" 后直接遇 & 或结尾）：跳过 */
            if (!sep) break;
            p = sep + 1;
            continue;
        }
        size_t klen = (size_t)(eq - p);
        const char *val = eq + 1;
        size_t vlen = sep ? (size_t)(sep - val) : strlen(val);
        if (klen == nlen && memcmp(p, name, nlen) == 0) {
            return url_decode_range(val, vlen, out, cap) >= 0;
        }
        p = sep ? sep + 1 : NULL;
    }
    return false;
}

void pomo_ap_ssid_from_mac(const uint8_t mac[6], char out[16]) {
    static const char HEX[] = "0123456789ABCDEF";
    memcpy(out, "WHIPLASH-", 9);
    out[9] = HEX[mac[4] >> 4];
    out[10] = HEX[mac[4] & 0xF];
    out[11] = HEX[mac[5] >> 4];
    out[12] = HEX[mac[5] & 0xF];
    out[13] = '\0';
}
