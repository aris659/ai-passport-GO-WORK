#include "wifi_provision.h"

#include <stddef.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "pomodoro_time.h"

static const char *TAG = "pomo_prov";

#define PROV_IDLE_TIMEOUT_MS (5ULL * 60 * 1000)   /* 无操作自动关 AP */
#define BANNER_WINDOW_MS     (15ULL * 60 * 1000)  /* 保存后结果横幅窗口 */
#define POST_SAVE_KEEP_MS    (2ULL * 1000)        /* 让响应可靠送达 */
#define MAX_BODY             512

/* ---- 内嵌页面（无外部资源，黑底白字红标题，与设备视觉一致） ---- */

static const char PAGE_INDEX[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>WHIPLASH</title><style>"
    "body{background:#050608;color:#F4F1E8;font-family:monospace;"
    "text-align:center;padding:24px;margin:0}"
    "h1{color:#F02B24;letter-spacing:2px;margin:8px 0 4px}"
    "p{color:#777C85}input{width:210px;max-width:80vw;padding:8px;"
    "background:#14171C;color:#F4F1E8;border:1px solid #777C85;"
    "font-size:16px;text-align:center}"
    "button{padding:10px 18px;margin-top:18px;background:#F02B24;"
    "color:#fff;border:0;font-size:15px;font-weight:bold}"
    "</style></head><body><h1>WHIPLASH</h1><p>WIFI SETUP</p>"
    "<form method=\"post\" action=\"/save\">"
    "SSID<br><input name=\"ssid\" maxlength=\"32\"><br><br>"
    "PASSWORD<br><input name=\"password\" type=\"password\" maxlength=\"63\"><br>"
    "<button>SAVE &amp; CONNECT</button></form>"
    "<p>2.4 GHz WiFi only</p></body></html>";

static const char PAGE_SAVED[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>WHIPLASH</title></head>"
    "<body style=\"background:#050608;color:#F4F1E8;font-family:monospace;"
    "text-align:center;padding:24px\"><h1 style=\"color:#F02B24\">WHIPLASH</h1>"
    "<p>Saved.<br>WHIPLASH is connecting to your WiFi.<br>"
    "You may close this page.</p></body></html>";

static const char PAGE_INVALID[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>WHIPLASH</title></head>"
    "<body style=\"background:#050608;color:#F4F1E8;font-family:monospace;"
    "text-align:center;padding:24px\"><h1 style=\"color:#F02B24\">WHIPLASH</h1>"
    "<p>Invalid SSID or password.<br>"
    "SSID: 1-32 chars. Password: 8-63 chars (empty = open)."
    "</p><p><a href=\"/\" style=\"color:#F02B24\">BACK</a></p></body></html>";

/* ---- 状态 ---- */

static SemaphoreHandle_t s_start_sem;   /* 请求启动会话 */
static SemaphoreHandle_t s_save_sem;    /* POST /save 完成 */
static volatile uint32_t s_last_active_ms;
static pomo_prov_state_t s_state = POMO_PROV_IDLE;
static char s_ap_ssid[16];
static httpd_handle_t s_httpd;
static uint64_t s_session_end_ms;
static bool s_ap_netif_created;

static uint64_t uptime_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

/* ---- NVS 用户凭据 ---- */

bool pomo_wifi_user_creds_load(char *ssid, size_t ssid_cap,
                               char *pass, size_t pass_cap) {
    if (!ssid || !pass || ssid_cap == 0 || pass_cap == 0) return false;
    ssid[0] = '\0';
    pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("whiplash_wifi", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = true;
    size_t len = ssid_cap;
    if (nvs_get_str(h, "ssid", ssid, &len) != ESP_OK || ssid[0] == '\0') {
        ok = false;
    } else {
        len = pass_cap;
        if (nvs_get_str(h, "pass", pass, &len) != ESP_OK) ok = false;
    }
    nvs_close(h);
    return ok;
}

bool pomo_wifi_user_creds_save(const char *ssid, const char *pass) {
    if (!ssid || !pass) return false;
    nvs_handle_t h;
    if (nvs_open("whiplash_wifi", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, "ssid", ssid);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

void pomo_wifi_user_creds_clear(void) {
    nvs_handle_t h;
    if (nvs_open("whiplash_wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

bool pomo_wifi_user_creds_exist(void) {
    char ssid[33], pass[64];
    return pomo_wifi_user_creds_load(ssid, sizeof(ssid), pass, sizeof(pass));
}

/* ---- HTTP 处理 ---- */

static void touch_activity(void) {
    s_last_active_ms = (uint32_t)uptime_ms();
}

static esp_err_t index_get_handler(httpd_req_t *req) {
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_INDEX, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    touch_activity();
    int total = (int)req->content_len;
    if (total <= 0 || total > MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }
    char body[MAX_BODY + 1];
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret <= 0) return ESP_FAIL;
        received += ret;
    }
    body[received] = '\0';

    char ssid[33], pass[64];
    if (!pomo_form_field(body, "ssid", ssid, sizeof(ssid)) ||
        !pomo_form_field(body, "password", pass, sizeof(pass)) ||
        !pomo_wifi_creds_valid(ssid, pass)) {
        /* 校验失败不视为会话结束：让用户在本页重试 */
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PAGE_INVALID, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!pomo_wifi_user_creds_save(ssid, pass)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_FAIL;
    }
    /* 只打 SSID，绝不打密码 */
    ESP_LOGI(TAG, "User WiFi config saved (ssid=\"%s\")", ssid);
    httpd_resp_set_type(req, "text/html");
    esp_err_t r = httpd_resp_send(req, PAGE_SAVED, HTTPD_RESP_USE_STRLEN);
    xSemaphoreGive(s_save_sem);
    return r;
}

static const httpd_uri_t URI_INDEX = {
    .uri = "/", .method = HTTP_GET, .handler = index_get_handler,
};
static const httpd_uri_t URI_SAVE = {
    .uri = "/save", .method = HTTP_POST, .handler = save_post_handler,
};

/* ---- 会话任务 ---- */

static void apply_state(pomo_prov_ev_t ev) {
    static const char *NAMES[] = { "idle", "active", "saved" };
    pomo_prov_state_t next = pomo_prov_next(s_state, ev);
    if (next != s_state) {
        s_state = next;
        ESP_LOGI(TAG, "Prov state -> %s", NAMES[next]);
    }
}

static bool start_httpd(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 4;
    cfg.stack_size = 4096;
    cfg.max_uri_handlers = 4;
    cfg.ctrl_port = 32768;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) return false;
    if (httpd_register_uri_handler(s_httpd, &URI_INDEX) != ESP_OK ||
        httpd_register_uri_handler(s_httpd, &URI_SAVE) != ESP_OK) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        return false;
    }
    return true;
}

static void run_session(void) {
    /* 与校时任务互斥：拿不到驱动说明对方正在连 SNTP，稍后再试 */
    if (!pomodoro_time_wifi_suspend(180000)) {
        ESP_LOGW(TAG, "WiFi driver busy; setup aborted, try again later");
        return;
    }

    apply_state(POMO_PROV_EV_START);
    /* 活动计时从会话开始：5 分钟无 HTTP 交互即自动关 AP 节电 */
    s_last_active_ms = (uint32_t)uptime_ms();
    ESP_LOGI(TAG, "Provisioning AP \"%s\" started (open 192.168.4.1)", s_ap_ssid);

    do {
        if (!s_ap_netif_created) {
            esp_netif_create_default_wifi_ap();
            s_ap_netif_created = true;
        }
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode(AP) failed: %s",
                     esp_err_to_name(err));
            break;
        }

        wifi_config_t ap = { 0 };
        strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = 0;
        ap.ap.channel = 6;
        ap.ap.authmode = WIFI_AUTH_OPEN;
        ap.ap.max_connection = 2;
        ap.ap.ssid_hidden = 0;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s",
                     esp_err_to_name(err));
            break;
        }
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start(AP) failed: %s",
                     esp_err_to_name(err));
            break;
        }

        if (!start_httpd()) {
            ESP_LOGE(TAG, "HTTP server start failed");
            break;
        }

        /* 等待保存或 5 分钟无操作超时（1 秒粒度轮询活动时间） */
        bool saved = false;
        while (true) {
            if (xSemaphoreTake(s_save_sem, pdMS_TO_TICKS(1000)) == pdTRUE) {
                saved = true;
                break;
            }
            uint32_t idle = (uint32_t)uptime_ms() - s_last_active_ms;
            if (idle >= PROV_IDLE_TIMEOUT_MS) {
                break;
            }
        }

        if (saved) {
            apply_state(POMO_PROV_EV_SAVE);
            vTaskDelay(pdMS_TO_TICKS(POST_SAVE_KEEP_MS));
        } else {
            apply_state(POMO_PROV_EV_TIMEOUT);
        }

        httpd_stop(s_httpd);
        s_httpd = NULL;
    } while (false);

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
    }
    s_session_end_ms = uptime_ms();

    pomodoro_time_wifi_resume();
    /* 保存了新凭据：立即唤醒校时任务尝试连接 */
    if (s_state == POMO_PROV_SAVED) pomodoro_time_reload_credentials();
}

static void prov_task(void *arg) {
    (void)arg;
    while (true) {
        if (xSemaphoreTake(s_start_sem, portMAX_DELAY) == pdTRUE) {
            run_session();
        }
    }
}

bool pomo_wifi_prov_start(void) {
    if (!s_start_sem) return false;
    if (s_state == POMO_PROV_ACTIVE) return false;
    /* 信号量满则说明已有待处理请求 */
    if (uxSemaphoreGetCount(s_start_sem) > 0) return true;
    return xSemaphoreGive(s_start_sem) == pdTRUE;
}

pomo_prov_state_t pomo_wifi_prov_state(void) {
    return s_state;
}

const char *pomo_wifi_prov_ap_ssid(void) {
    return s_ap_ssid;
}

bool pomo_wifi_prov_show_banner(void) {
    if (s_state == POMO_PROV_ACTIVE) return true;
    return s_state == POMO_PROV_SAVED &&
           uptime_ms() - s_session_end_ms < BANNER_WINDOW_MS;
}

void pomo_wifi_prov_early_init(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    pomo_ap_ssid_from_mac(mac, s_ap_ssid);

    s_start_sem = xSemaphoreCreateBinary();
    s_save_sem = xSemaphoreCreateBinary();
    if (!s_start_sem || !s_save_sem) {
        ESP_LOGE(TAG, "Failed to create prov semaphores");
        return;
    }
    if (xTaskCreate(prov_task, "pomo_prov", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create prov task");
    }
}
