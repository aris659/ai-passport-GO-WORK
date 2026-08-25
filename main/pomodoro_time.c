#include "pomodoro_time.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pomodoro_date.h"
#include "pomodoro_stats.h"
#include "wifi_config.h"
#include "wifi_provision.h"

#define WIFI_CONNECT_TIMEOUT_MS (15 * 1000)  /* 单凭据超时；多凭据总最坏 2x */
#define WIFI_SYNC_WINDOW_MS     (60 * 1000)
#define WIFI_POST_SYNC_KEEP_MS  (10 * 1000)
#define WIFI_CONNECT_ATTEMPTS   3
#define RETRY_AFTER_FAIL_MS     (30LL * 60 * 1000)
/* 正常重同步周期 6 小时：番茄钟无亚秒级精度需求，减少 WiFi 唤醒省电。 */
#define RECHECK_PERIOD_MS       (6LL * 60 * 60 * 1000)
/* 拿驱动锁的等待上限：配网会话最长 5 分钟超时 + 余量。 */
#define DRV_LOCK_TIMEOUT_MS     (8LL * 60 * 1000)

#define MAX_CREDS 3  /* NVS 用户配置 1 条 + 编译期 wifi_config.h 2 条 */

typedef struct {
    char ssid[33];
    char pass[64];
} wifi_cred_t;

static const char *TAG = "pomo_time";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pomo_time_status_t s_status = POMO_TIME_NONE;
static int64_t s_anchor_unix = 0;
static uint64_t s_anchor_uptime_ms = 0;
static volatile pomo_wifi_state_t s_wifi_state = POMO_WIFI_STATE_OFFLINE;

static SemaphoreHandle_t s_sync_sem;   /* SNTP 完成信号 */
static SemaphoreHandle_t s_drv_sem;    /* WiFi 驱动所有权：校时任务与配网会话互斥 */
static SemaphoreHandle_t s_kick_sem;   /* 凭据变化：唤醒校时任务立即重试 */
static EventGroupHandle_t s_wifi_events;
static volatile uint8_t s_connect_attempts;

static uint64_t uptime_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connect_attempts < WIFI_CONNECT_ATTEMPTS) {
            s_connect_attempts++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void sntp_sync_cb(struct timeval *tv) {
    (void)tv;
    if (s_sync_sem) xSemaphoreGive(s_sync_sem);
}

static void mark_synced(void) {
    int64_t now = (int64_t)time(NULL);
    /* 防 SNTP 异常值：早于 2001-09-09 视为无效。 */
    if (now < 1000000000LL) return;
    portENTER_CRITICAL(&s_lock);
    s_anchor_unix = now;
    s_anchor_uptime_ms = uptime_ms();
    s_status = POMO_TIME_SYNCED;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Time synced via SNTP");
}

/* 每轮尝试前重新装配凭据表：NVS 用户配置（SoftAP 配网）优先，
 * 编译期 wifi_config.h 次之。配网保存后无需重启即可生效。 */
static int load_creds(wifi_cred_t *out) {
    int n = 0;
    char ssid[33], pass[64];
    if (pomo_wifi_user_creds_load(ssid, sizeof(ssid), pass, sizeof(pass)) &&
        ssid[0] != '\0') {
        strlcpy(out[n].ssid, ssid, sizeof(out[n].ssid));
        strlcpy(out[n].pass, pass, sizeof(out[n].pass));
        n++;
    }
#if defined(POMO_WIFI_SSID_1)
    if (POMO_WIFI_SSID_1[0] != '\0' && n < MAX_CREDS) {
        strlcpy(out[n].ssid, POMO_WIFI_SSID_1, sizeof(out[n].ssid));
        strlcpy(out[n].pass, POMO_WIFI_PASS_1, sizeof(out[n].pass));
        n++;
    }
#endif
#if defined(POMO_WIFI_SSID_2)
    if (POMO_WIFI_SSID_2[0] != '\0' && n < MAX_CREDS) {
        strlcpy(out[n].ssid, POMO_WIFI_SSID_2, sizeof(out[n].ssid));
        strlcpy(out[n].pass, POMO_WIFI_PASS_2, sizeof(out[n].pass));
        n++;
    }
#endif
    return n;
}

static bool has_build_creds(void) {
#if defined(POMO_WIFI_SSID_1)
    if (POMO_WIFI_SSID_1[0] != '\0') return true;
#endif
#if defined(POMO_WIFI_SSID_2)
    if (POMO_WIFI_SSID_2[0] != '\0') return true;
#endif
    return false;
}

/* 一次完整尝试：遍历凭据表，逐个 连 WiFi -> SNTP 同步 -> 关闭。
 * 网络是可选子系统：任一步失败只记录日志并返回 false，
 * 保留既有 ESTIMATE/NONE 时间状态，等待下个重试窗口，绝不 abort。 */
static bool try_cred_once(const wifi_cred_t *cred) {
    s_connect_attempts = 0;

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, cred->ssid,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, cred->pass,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    bool connected = (bits & WIFI_CONNECTED_BIT) != 0;

    bool synced = false;
    if (connected) {
        xSemaphoreTake(s_sync_sem, 0);  /* 清掉可能的旧信号 */
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "pool.ntp.org");
        esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
        esp_sntp_set_sync_interval(60 * 1000);
        esp_sntp_init();

        if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(WIFI_SYNC_WINDOW_MS)) ==
            pdTRUE) {
            mark_synced();
            synced = true;
            /* 同步后再保持一小段，吸收后续 SNTP 采样。 */
            vTaskDelay(pdMS_TO_TICKS(WIFI_POST_SYNC_KEEP_MS));
        }
        esp_sntp_stop();
    }

    xEventGroupClearBits(s_wifi_events,
                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }
    return synced;
}

static bool try_sync_once(const wifi_cred_t *creds, int n) {
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "Trying WiFi \"%s\"", creds[i].ssid);
        if (try_cred_once(&creds[i])) return true;
    }
    return false;
}

/* 常驻校时任务：成功后 6h 重同步，失败 30min 重试；
 * 无凭据时休眠等配网 kick。驱动锁与配网会话互斥。 */
static void wifi_time_task(void *arg) {
    (void)arg;
    bool first = true;
    while (true) {
        wifi_cred_t creds[MAX_CREDS];
        int n = 0;
        if (first) {
            first = false;
            n = load_creds(creds);   /* 启动后立即首轮 */
        } else {
            TickType_t wait;
            if (s_wifi_state == POMO_WIFI_STATE_CONNECTED) {
                wait = pdMS_TO_TICKS(RECHECK_PERIOD_MS);
            } else if (s_wifi_state == POMO_WIFI_STATE_OFFLINE) {
                wait = portMAX_DELAY;   /* 无凭据：等配网保存唤醒 */
            } else {
                wait = pdMS_TO_TICKS(RETRY_AFTER_FAIL_MS);
            }
            xSemaphoreTake(s_kick_sem, wait);  /* 被 reload 提前唤醒 */
            n = load_creds(creds);
        }

        if (n == 0) {
            s_wifi_state = POMO_WIFI_STATE_OFFLINE;
            continue;
        }
        if (xSemaphoreTake(s_drv_sem, pdMS_TO_TICKS(DRV_LOCK_TIMEOUT_MS)) !=
            pdTRUE) {
            ESP_LOGW(TAG, "WiFi driver busy; sync deferred");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        s_wifi_state = POMO_WIFI_STATE_CONNECTING;
        bool ok = try_sync_once(creds, n);
        s_wifi_state = ok ? POMO_WIFI_STATE_CONNECTED
                          : POMO_WIFI_STATE_FAILED;
        xSemaphoreGive(s_drv_sem);
        if (ok) {
            ESP_LOGI(TAG, "Next SNTP resync in 6 h");
        } else {
            ESP_LOGW(TAG, "Time sync failed; retry in 30 min");
        }
    }
}

void pomodoro_time_init(int64_t anchor_unix) {
    static bool s_inited = false;
    if (s_inited) return;
    s_inited = true;

    if (anchor_unix > 0) {
        portENTER_CRITICAL(&s_lock);
        s_anchor_unix = anchor_unix;
        s_anchor_uptime_ms = uptime_ms();
        s_status = POMO_TIME_ESTIMATE;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Time base: estimate from anchor %lld",
                 (long long)anchor_unix);
    } else {
        ESP_LOGI(TAG, "Time base: none");
    }

    s_sync_sem = xSemaphoreCreateBinary();
    s_wifi_events = xEventGroupCreate();
    s_drv_sem = xSemaphoreCreateMutex();
    s_kick_sem = xSemaphoreCreateBinary();
    if (!s_sync_sem || !s_wifi_events || !s_drv_sem || !s_kick_sem) {
        ESP_LOGE(TAG, "Failed to create sync primitives");
        return;
    }

    /* 前置：pomodoro_store_init 已完成 nvs_flash_init。
     * WiFi 初始化失败属于可选子系统故障：记录日志、放弃时间同步，
     * 番茄钟本体（计时/统计/交互）继续运行，不 abort。 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s; SNTP disabled",
                 esp_err_to_name(err));
        return;
    }
    esp_event_loop_create_default();  /* 已存在时不视为错误 */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s; SNTP disabled",
                 esp_err_to_name(err));
        return;
    }
    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi event register failed: %s; SNTP disabled",
                 esp_err_to_name(err));
        return;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi mode/storage setup failed: %s; SNTP disabled",
                 esp_err_to_name(err));
        return;
    }

    /* 配网模块：缓存 AP SSID + 创建会话任务（幂等） */
    pomo_wifi_prov_early_init();

    /* 凭据来源日志：NVS(用户配网) > 编译期 wifi_config.h > 无 */
    bool nvs_has = pomo_wifi_user_creds_exist();
    bool build_has = has_build_creds();
    if (nvs_has) {
        ESP_LOGI(TAG, "WiFi credential source: NVS (user provisioned)");
    } else if (build_has) {
        ESP_LOGI(TAG, "WiFi credential source: compile-time wifi_config.h");
    } else {
        ESP_LOGI(TAG, "WiFi credentials: none");
    }

    if (xTaskCreate(wifi_time_task, "pomo_time", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create time sync task");
        return;
    }

    /* 公开固件无任何凭据：自动启动一次 SoftAP 配网。
     * 用户可按 OK 关闭提示离线使用，AP 5 分钟无操作自动关闭。 */
    if (!nvs_has && !build_has) {
        pomo_wifi_prov_start();
    }
}

pomo_time_status_t pomodoro_time_status(void) {
    portENTER_CRITICAL(&s_lock);
    pomo_time_status_t status = s_status;
    portEXIT_CRITICAL(&s_lock);
    return status;
}

int64_t pomodoro_time_now_unix(void) {
    portENTER_CRITICAL(&s_lock);
    pomo_time_status_t status = s_status;
    int64_t anchor = s_anchor_unix;
    uint64_t anchor_uptime = s_anchor_uptime_ms;
    portEXIT_CRITICAL(&s_lock);

    if (status == POMO_TIME_SYNCED) return (int64_t)time(NULL);
    if (status == POMO_TIME_ESTIMATE) {
        return pomo_time_estimate_unix(anchor, anchor_uptime, uptime_ms());
    }
    return 0;
}

uint16_t pomodoro_time_today(void) {
    int64_t now = pomodoro_time_now_unix();
    if (now <= 0) return POMO_NO_DATE;
    return pomo_date_from_unix(now);
}

pomo_wifi_state_t pomodoro_time_wifi_state(void) {
    return s_wifi_state;
}

bool pomodoro_time_wifi_suspend(uint32_t timeout_ms) {
    if (!s_drv_sem) return false;
    return xSemaphoreTake(s_drv_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void pomodoro_time_wifi_resume(void) {
    if (s_drv_sem) xSemaphoreGive(s_drv_sem);
}

void pomodoro_time_reload_credentials(void) {
    if (s_kick_sem) xSemaphoreGive(s_kick_sem);
}
