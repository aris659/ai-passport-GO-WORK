#include "pomodoro_store.h"

#include <stddef.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "pomodoro_blob.h"

static const char *TAG = "pomo_store";
static QueueHandle_t s_queue;
static nvs_handle_t s_nvs;
static bool s_ready;
static volatile bool s_error;

static void save_task(void *arg) {
    (void)arg;
    pomo_blob_v2_t blob;
    while (true) {
        if (xQueueReceive(s_queue, &blob, portMAX_DELAY) == pdTRUE) {
            esp_err_t err = nvs_set_blob(s_nvs, "state", &blob, sizeof(blob));
            if (err == ESP_OK) err = nvs_commit(s_nvs);
            if (err != ESP_OK) {
                s_error = true;
                ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
            }
        }
    }
}

bool pomodoro_store_init(pomodoro_model_t *model, pomo_stats_t *stats,
                         int64_t *anchor_unix) {
    if (!model || !stats || !anchor_unix) return false;
    if (s_ready) return true;

    pomodoro_model_defaults(model);
    pomo_stats_init(stats);
    *anchor_unix = 0;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs recovery: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        s_error = true;
        return false;
    }

    err = nvs_open("pomo", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        s_error = true;
        return false;
    }

    uint8_t buf[sizeof(pomo_blob_v2_t)];
    size_t size = sizeof(buf);
    err = nvs_get_blob(s_nvs, "state", buf, &size);
    if (err == ESP_OK) {
        pomo_blob_v2_t blob;
        switch (pomo_blob_decode(buf, size, model, stats, anchor_unix)) {
        case POMO_BLOB_DECODE_V1:
            /* 一次性迁移：立即落盘 v2，下次开机走正常路径。 */
            ESP_LOGI(TAG, "Migrating stored state v1 -> v2");
            pomo_blob_encode(&blob, model, stats, *anchor_unix);
            err = nvs_set_blob(s_nvs, "state", &blob, sizeof(blob));
            if (err == ESP_OK) err = nvs_commit(s_nvs);
            if (err != ESP_OK) {
                s_error = true;
                ESP_LOGE(TAG, "NVS migration write failed: %s",
                         esp_err_to_name(err));
            }
            break;
        case POMO_BLOB_DECODE_V2:
            ESP_LOGI(TAG, "Restored state: %lu pomos, %lu focus min, anchor %lld",
                     (unsigned long)stats->total_pomos,
                     (unsigned long)stats->total_focus_min,
                     (long long)*anchor_unix);
            break;
        default:
            ESP_LOGW(TAG, "Stored state invalid; using defaults");
            break;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "NVS load failed: %s", esp_err_to_name(err));
    }

    s_queue = xQueueCreate(1, sizeof(pomo_blob_v2_t));
    if (!s_queue || xTaskCreate(save_task, "pomo_save", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create persistence worker");
        s_error = true;
        return false;
    }
    s_ready = true;
    return true;
}

void pomodoro_store_request_save(const pomodoro_model_t *model,
                                 const pomo_stats_t *stats,
                                 int64_t anchor_unix) {
    if (!s_ready || !model || !stats) return;
    pomo_blob_v2_t blob;
    pomo_blob_encode(&blob, model, stats, anchor_unix);
    if (xQueueOverwrite(s_queue, &blob) != pdTRUE) s_error = true;
}

bool pomodoro_store_has_error(void) {
    return s_error;
}
