#include "wifi_prov_util.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_prov_state_machine(void) {
    /* IDLE: 只有 START 生效 */
    assert(pomo_prov_next(POMO_PROV_IDLE, POMO_PROV_EV_START) == POMO_PROV_ACTIVE);
    assert(pomo_prov_next(POMO_PROV_IDLE, POMO_PROV_EV_TIMEOUT) == POMO_PROV_IDLE);
    assert(pomo_prov_next(POMO_PROV_IDLE, POMO_PROV_EV_SAVE) == POMO_PROV_IDLE);

    /* ACTIVE: SAVE -> SAVED, TIMEOUT -> IDLE，其余保持 */
    assert(pomo_prov_next(POMO_PROV_ACTIVE, POMO_PROV_EV_SAVE) == POMO_PROV_SAVED);
    assert(pomo_prov_next(POMO_PROV_ACTIVE, POMO_PROV_EV_TIMEOUT) == POMO_PROV_IDLE);
    assert(pomo_prov_next(POMO_PROV_ACTIVE, POMO_PROV_EV_START) == POMO_PROV_ACTIVE);

    /* SAVED: 新 START 重开会话，其余保持（结果横幅窗口由 UI 侧控制） */
    assert(pomo_prov_next(POMO_PROV_SAVED, POMO_PROV_EV_START) == POMO_PROV_ACTIVE);
    assert(pomo_prov_next(POMO_PROV_SAVED, POMO_PROV_EV_TIMEOUT) == POMO_PROV_SAVED);
    assert(pomo_prov_next(POMO_PROV_SAVED, POMO_PROV_EV_SAVE) == POMO_PROV_SAVED);
}

static void test_src_priority(void) {
    /* NVS > 编译期 > 无 */
    assert(pomo_wifi_src_pick(true, true) == POMO_WIFI_SRC_NVS);
    assert(pomo_wifi_src_pick(true, false) == POMO_WIFI_SRC_NVS);
    assert(pomo_wifi_src_pick(false, true) == POMO_WIFI_SRC_BUILD);
    assert(pomo_wifi_src_pick(false, false) == POMO_WIFI_SRC_NONE);
}

static void test_creds_valid(void) {
    assert(pomo_wifi_creds_valid("Home", "12345678"));
    assert(pomo_wifi_creds_valid("Home", ""));              /* 开放网络 */
    assert(pomo_wifi_creds_valid("A", "12345678"));         /* 1 字节 SSID */

    char ssid33[34];
    memset(ssid33, 'S', 33);
    ssid33[33] = '\0';
    assert(!pomo_wifi_creds_valid(ssid33, "12345678"));     /* SSID > 32 */
    assert(!pomo_wifi_creds_valid("", "12345678"));         /* 空 SSID */
    assert(!pomo_wifi_creds_valid("Home", "1234567"));      /* WPA < 8 */

    char pass64[66];
    memset(pass64, 'P', 63);
    pass64[63] = '\0';
    assert(pomo_wifi_creds_valid("Home", pass64));          /* 恰好 63 合法 */
    pass64[63] = 'P';
    pass64[64] = '\0';
    assert(!pomo_wifi_creds_valid("Home", pass64));         /* 64 越界 */

    assert(!pomo_wifi_creds_valid(NULL, "12345678"));
    assert(!pomo_wifi_creds_valid("Home", NULL));
}

static void test_url_decode(void) {
    char buf[64];

    strcpy(buf, "My+Net");
    assert(pomo_url_decode(buf, sizeof(buf)) == 6);
    assert(strcmp(buf, "My Net") == 0);

    strcpy(buf, "p%40ss%2Fword");
    assert(pomo_url_decode(buf, sizeof(buf)) == 9);
    assert(strcmp(buf, "p@ss/word") == 0);

    strcpy(buf, "%41%42%43");
    assert(pomo_url_decode(buf, sizeof(buf)) == 3);
    assert(strcmp(buf, "ABC") == 0);

    /* 非法 %XX */
    strcpy(buf, "%G1");
    assert(pomo_url_decode(buf, sizeof(buf)) == -1);
    strcpy(buf, "abc%");
    assert(pomo_url_decode(buf, sizeof(buf)) == -1);

    /* 解码后装不下（cap=3 只容 2 字符 + NUL） */
    strcpy(buf, "abcd");
    assert(pomo_url_decode(buf, 3) == -1);
}

static void test_form_field(void) {
    char out[64];

    assert(pomo_form_field("ssid=Home&password=12345678", "ssid",
                           out, sizeof(out)));
    assert(strcmp(out, "Home") == 0);
    assert(pomo_form_field("ssid=Home&password=12345678", "password",
                           out, sizeof(out)));
    assert(strcmp(out, "12345678") == 0);

    /* URL 编码值 */
    assert(pomo_form_field("ssid=My+%26+Net&password=x", "ssid",
                           out, sizeof(out)));
    assert(strcmp(out, "My & Net") == 0);

    /* 空密码（开放网络） */
    assert(pomo_form_field("ssid=OpenNet&password=", "password",
                           out, sizeof(out)));
    assert(strcmp(out, "") == 0);

    /* 字段缺失 / 顺序无关 / 前缀不误匹配 */
    assert(!pomo_form_field("password=x", "ssid", out, sizeof(out)));
    assert(!pomo_form_field("ssid2=abc&password=x", "ssid", out, sizeof(out)));
    assert(pomo_form_field("a=1&ssid=Tail&b=2", "ssid", out, sizeof(out)));
    assert(strcmp(out, "Tail") == 0);

    /* 超长拒绝 */
    assert(!pomo_form_field("ssid=0123456789ABCDEF", "ssid", out, 8));

    /* 只有键没有值的字段跳过，后面的字段仍可解析 */
    assert(pomo_form_field("ssid&password=px", "password", out, sizeof(out)));
    assert(strcmp(out, "px") == 0);
}

static void test_ap_ssid(void) {
    char ssid[16];
    uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0xA3, 0xF2};
    pomo_ap_ssid_from_mac(mac, ssid);
    assert(strcmp(ssid, "WHIPLASH-A3F2") == 0);

    uint8_t mac2[6] = {0, 0, 0, 0, 0x00, 0x0F};
    pomo_ap_ssid_from_mac(mac2, ssid);
    assert(strcmp(ssid, "WHIPLASH-000F") == 0);

    uint8_t mac3[6] = {0, 0, 0, 0, 0xFF, 0xFF};
    pomo_ap_ssid_from_mac(mac3, ssid);
    assert(strcmp(ssid, "WHIPLASH-FFFF") == 0);
}

int main(void) {
    test_prov_state_machine();
    test_src_priority();
    test_creds_valid();
    test_url_decode();
    test_form_field();
    test_ap_ssid();
    printf("test_wifi_prov PASS\n");
    return 0;
}
