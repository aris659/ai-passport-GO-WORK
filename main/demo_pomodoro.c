#include "demo.h"

#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "battery_gauge.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pomodoro_date.h"
#include "pomodoro_model.h"
#include "pomodoro_stats.h"
#include "pomodoro_store.h"
#include "pomodoro_time.h"
#include "wifi_provision.h"

#define COLOR_BG       0x050608
#define COLOR_WHITE    0xF4F1E8
#define COLOR_DIM      0x777C85
#define COLOR_TRACK    0x25282E
#define COLOR_RED      0xF02B24
#define COLOR_RED_DARK 0x9D1718
#define COLOR_RED_DEAD 0x5A120E
#define COLOR_RED_BEAT 0xFF5A47
#define COLOR_GREEN    0x36D765
#define COLOR_YELLOW   0xFFD65A

#define TONE_SAMPLE_RATE 16000
#define TONE_CHUNK       128

typedef enum {
    TONE_START = 1,
    TONE_PAUSE,
    TONE_TRIPLE,
} tone_id_t;

typedef enum {
    BL_SCENE_IDLE = 0,
    BL_SCENE_FOCUS,
    BL_SCENE_FOCUS_PAUSED,
    BL_SCENE_CONFIRM,
    BL_SCENE_REWARD,
    BL_SCENE_BREAK_PROMPT,
    BL_SCENE_BREAK,
    BL_SCENE_BREAK_PAUSED,
    BL_SCENE_STATS,
    BL_SCENE_COUNT,
} bl_scene_t;

static const char *TAG = "pomo";
static const char *WEEKDAY_NAMES[] = {
    "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"
};

/* 电池状态机：bsp_battery_init()==ESP_OK 只代表 CW2017 在线应答。
 * SOC raw 在 0..100 范围内不等于可信——raw=0x0000 是芯片上电默认值，
 * 与 VCELL>=3.6V 明显矛盾（见 battery_gauge.c）。
 * 就绪判定 = 连续 3 个可信样本，判定逻辑在 main/battery_gauge.c（host 可测）。 */
typedef enum {
    BATT_UNAVAILABLE = 0, /* 0x63 无应答：按 1s/3s/10s/60s 重试 init */
    BATT_WAIT_READY,      /* 快速观察：250ms 轮询，10s 窗口；连续 3 可信 → READY */
    BATT_READY,           /* SOC 可信：30s 轮询；连续 3 异常 → FALLBACK */
    BATT_FALLBACK,        /* 显示实测电压；5s 低频读 SOC；仅在 CONFIG 异常等
                             有证据时 restart（≤2 次/boot，60s cooldown） */
} batt_state_t;

#define BATT_FAST_POLL_MS        250         /* WAIT_READY 轮询间隔 */
#define BATT_READY_WINDOW_MS     (10ULL*1000)/* WAIT_READY 观察窗口 */
#define BATT_READY_POLL_MS       (30ULL*1000)/* READY 轮询间隔 */
#define BATT_FALLBACK_POLL_MS    (5ULL*1000) /* FALLBACK 低频读 SOC 间隔 */
#define BATT_RESTART_COOLDOWN_MS (60ULL*1000)
#define BATT_MAX_RESTARTS        2           /* 一次 boot 最多自动 restart 次数 */

static pomodoro_model_t s_model;
static pomo_stats_t s_stats;
static int64_t s_anchor_unix;
static bool s_prepared;
static bool s_audio_ok;
static batt_state_t s_battery_state = BATT_UNAVAILABLE;
static QueueHandle_t s_audio_queue;
static volatile bool s_audio_cancel;

static lv_obj_t *s_scr;
static lv_obj_t *s_main_layer;
static lv_obj_t *s_stats_layer;

/* 主视图元素 */
static lv_obj_t *s_top_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_battery_icon;
static lv_obj_t *s_brand;
static lv_obj_t *s_clock_layer;
static lv_obj_t *s_gowork_layer;
static lv_obj_t *s_heart_layer;
static lv_obj_t *s_mmss_label;
static lv_obj_t *s_plus_label;
static lv_obj_t *s_done_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_action_icon;

/* 统计页元素 */
static lv_obj_t *s_stats_today;
static lv_obj_t *s_chart_layer;
static lv_obj_t *s_stats_weekday[7];
static lv_obj_t *s_stats_week;
static lv_obj_t *s_stats_all;
static uint16_t s_chart_min[7];

static lv_timer_t *s_timer;
static bool s_stats_view;
static uint64_t s_stats_open_ms;
static bg_ctx_t s_bg;                  /* SOC readiness 纯逻辑状态机 */
static int s_battery_soc = -1;         /* bg_display_soc() 镜像，供文案/图标读 */
static int s_battery_mv = -1;          /* EMA 平滑电压（fallback 文案/图标粗估用） */
static uint8_t s_batt_mv_fail_streak;  /* 连续电压读失败：清空冻结的电压显示 */
static int s_batt_volt_shown = -1;     /* 上次刷进 UI 的电压值（节流） */
static uint64_t s_batt_volt_ui_ms;     /* 上次电压 UI 刷新时刻 */
static uint64_t s_batt_next_poll_ms;   /* 下次轮询时刻（按状态取不同间隔） */
static uint64_t s_batt_deadline_ms;    /* WAIT_READY 观察窗口截止 */
static uint64_t s_batt_recovery_ms;    /* 上次有证据的 gauge restart；0=从未 */
static uint8_t s_batt_restart_count;   /* 一次 boot 已 restart 次数 */
static bool s_batt_logged_first;       /* WAIT_READY 首样本详情只打一次 */
static uint64_t s_batt_retry_ms;       /* UNAVAILABLE 时 init 重试时刻 */
static uint8_t s_batt_retry_count;     /* 1s/3s/10s 后转 60s 周期 */
static uint32_t s_last_sec = UINT32_MAX;
static uint32_t s_last_minute = UINT32_MAX;
static uint32_t s_save_bucket = UINT32_MAX;
static pomo_time_status_t s_last_time_status = POMO_TIME_NONE;
static uint64_t s_last_hourly_ms;

/* 闲时像素时钟状态 */
static int s_clock_hh;
static int s_clock_mm;
static bool s_clock_valid;
static bool s_colon_on = true;
static uint32_t s_blink_bucket = UINT32_MAX;
static uint16_t s_idle_whips;
static uint16_t s_idle_min;
static bool s_idle_data_ok;
static char s_idle_date[16];

/* GO WORK!! 街机警告体动画状态 */
typedef struct {
    uint8_t frame;     /* 当前帧；GW_FRAME_REST = 静止 */
} gw_anim_t;
static gw_anim_t s_gw;
static uint64_t s_gw_next_ms;   /* 下次咆哮开始时刻 */
static uint64_t s_gw_end_ms;    /* 当前咆哮结束时刻 */
static bool s_gw_active;

/* WiFi 配网横幅（闲时覆盖层） */
static lv_obj_t *s_wifi_layer;
static lv_obj_t *s_wifi_title;
static lv_obj_t *s_wifi_l1;
static lv_obj_t *s_wifi_l2;
static lv_obj_t *s_wifi_l3;
static lv_obj_t *s_wifi_l4;
static lv_obj_t *s_wifi_l5;
static bool s_wifi_banner_on;
static bool s_wifi_banner_dismissed;
static pomo_prov_state_t s_prev_prov_state = POMO_PROV_IDLE;
static uint8_t s_wifi_banner_sig = 0xFF;
static uint64_t s_last_wifi_check_ms;

/* 计时屏血心状态 */
static float s_heart_frac;        /* 血量 0..1：专注=剩余，休息=已回补 */
static bool s_heart_stopped;      /* 暂停=心脏停跳（暗红血量） */
static bool s_heart_beat;
static uint64_t s_heart_beat_until;
static bool s_heart_flash_on;

/* 背光状态机 */
static bl_scene_t s_bl_scene = BL_SCENE_IDLE;
static uint64_t s_bl_ref_ms;
static int s_bl_current = 100;
static bool s_screen_off;

static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

/* 电量文案：READY → "NN%"；FALLBACK 且平滑电压落在 LiPo 合理区间 → 实测电压；
 * 其余（不可用/等待就绪）→ "--%"。不把电压换算成看似精确的百分比。 */
static void battery_text(char *buf, size_t len) {
    if (s_battery_soc >= 0) {
        snprintf(buf, len, "%d%%", s_battery_soc);
    } else if (s_battery_state == BATT_FALLBACK &&
               s_battery_mv >= 2500 && s_battery_mv <= 4500) {
        snprintf(buf, len, "%d.%02dV", s_battery_mv / 1000,
                 (s_battery_mv % 1000) / 10);
    } else {
        snprintf(buf, len, "--%%");
    }
}

static lv_obj_t *container(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font,
                       uint32_t color) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *centered_label(lv_obj_t *parent, const lv_font_t *font,
                                uint32_t color, int y) {
    lv_obj_t *obj = label(parent, font, color);
    lv_obj_set_width(obj, 240);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(obj, 0, y);
    return obj;
}

static void draw_pixel_rect(lv_layer_t *layer, const lv_area_t *base,
                            int x, int y, int w, int h,
                            uint32_t color, int radius) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_area_t area = {
        .x1 = base->x1 + x,
        .y1 = base->y1 + y,
        .x2 = base->x1 + x + w - 1,
        .y2 = base->y1 + y + h - 1,
    };
    lv_draw_rect(layer, &dsc, &area);
}

static lv_obj_t *draw_layer_create(lv_obj_t *parent, int x, int y, int w, int h,
                                   lv_event_cb_t draw_cb) {
    lv_obj_t *obj = container(parent, x, y, w, h);
    lv_obj_add_event_cb(obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return obj;
}

static void set_hidden(lv_obj_t *obj, bool hidden) {
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *brand_create(lv_obj_t *parent) {
    enum { BRAND_WIDTH = 134, BRAND_HEIGHT = 21, BRAND_STRIDE = 17 };
    static const uint8_t BRAND_BITS[] = {
        0xE0, 0x0E, 0x70, 0x07, 0x07, 0xFC, 0x1F, 0xFE, 0x0E, 0x00, 0x00, 0xFF, 0x80,
        0x7F, 0xF9, 0xC0, 0x1C, 0xE0, 0x0E, 0x70, 0x07, 0x07, 0xFC, 0x1F, 0xFE, 0x0E,
        0x00, 0x00, 0xFF, 0x80, 0x7F, 0xF9, 0xC0, 0x1C, 0xE0, 0x0E, 0x70, 0x07, 0x07,
        0xFC, 0x1F, 0xFE, 0x0E, 0x00, 0x00, 0xFF, 0x80, 0x7F, 0xF9, 0xC0, 0x1C, 0xE0,
        0x0E, 0x70, 0x07, 0x00, 0xE0, 0x1C, 0x01, 0xCE, 0x00, 0x07, 0x00, 0x73, 0x80,
        0x01, 0xC0, 0x1C, 0xE0, 0x0E, 0x70, 0x07, 0x00, 0xE0, 0x1C, 0x01, 0xCE, 0x00,
        0x07, 0x00, 0x73, 0x80, 0x01, 0xC0, 0x1C, 0xE0, 0x0E, 0x70, 0x07, 0x00, 0xE0,
        0x1C, 0x01, 0xCE, 0x00, 0x07, 0x00, 0x73, 0x80, 0x01, 0xC0, 0x1C, 0xE0, 0x0E,
        0x70, 0x07, 0x00, 0xE0, 0x1C, 0x01, 0xCE, 0x00, 0x07, 0x00, 0x73, 0x80, 0x01,
        0xC0, 0x1C, 0xE0, 0x0E, 0x70, 0x07, 0x00, 0xE0, 0x1C, 0x01, 0xCE, 0x00, 0x07,
        0x00, 0x73, 0x80, 0x01, 0xC0, 0x1C, 0xE0, 0x0E, 0x70, 0x07, 0x00, 0xE0, 0x1C,
        0x01, 0xCE, 0x00, 0x07, 0x00, 0x73, 0x80, 0x01, 0xC0, 0x1C, 0xE3, 0x8E, 0x7F,
        0xFF, 0x00, 0xE0, 0x1F, 0xFE, 0x0E, 0x00, 0x07, 0xFF, 0xF0, 0x7F, 0xC1, 0xFF,
        0xFC, 0xE3, 0x8E, 0x7F, 0xFF, 0x00, 0xE0, 0x1F, 0xFE, 0x0E, 0x00, 0x07, 0xFF,
        0xF0, 0x7F, 0xC1, 0xFF, 0xFC, 0xE3, 0x8E, 0x7F, 0xFF, 0x00, 0xE0, 0x1F, 0xFE,
        0x0E, 0x00, 0x07, 0xFF, 0xF0, 0x7F, 0xC1, 0xFF, 0xFC, 0xE3, 0x8E, 0x70, 0x07,
        0x00, 0xE0, 0x1C, 0x00, 0x0E, 0x00, 0x07, 0x00, 0x70, 0x00, 0x39, 0xC0, 0x1C,
        0xE3, 0x8E, 0x70, 0x07, 0x00, 0xE0, 0x1C, 0x00, 0x0E, 0x00, 0x07, 0x00, 0x70,
        0x00, 0x39, 0xC0, 0x1C, 0xE3, 0x8E, 0x70, 0x07, 0x00, 0xE0, 0x1C, 0x00, 0x0E,
        0x00, 0x07, 0x00, 0x70, 0x00, 0x39, 0xC0, 0x1C, 0xFC, 0x7E, 0x70, 0x07, 0x00,
        0xE0, 0x1C, 0x00, 0x0E, 0x00, 0x07, 0x00, 0x70, 0x00, 0x39, 0xC0, 0x1C, 0xFC,
        0x7E, 0x70, 0x07, 0x00, 0xE0, 0x1C, 0x00, 0x0E, 0x00, 0x07, 0x00, 0x70, 0x00,
        0x39, 0xC0, 0x1C, 0xFC, 0x7E, 0x70, 0x07, 0x00, 0xE0, 0x1C, 0x00, 0x0E, 0x00,
        0x07, 0x00, 0x70, 0x00, 0x39, 0xC0, 0x1C, 0x1C, 0x70, 0x70, 0x07, 0x07, 0xFC,
        0x1C, 0x00, 0x0F, 0xFF, 0xE7, 0x00, 0x73, 0xFF, 0xC1, 0xC0, 0x1C, 0x1C, 0x70,
        0x70, 0x07, 0x07, 0xFC, 0x1C, 0x00, 0x0F, 0xFF, 0xE7, 0x00, 0x73, 0xFF, 0xC1,
        0xC0, 0x1C, 0x1C, 0x70, 0x70, 0x07, 0x07, 0xFC, 0x1C, 0x00, 0x0F, 0xFF, 0xE7,
        0x00, 0x73, 0xFF, 0xC1, 0xC0, 0x1C,
    };
    LV_DRAW_BUF_DEFINE_STATIC(brand_buf, BRAND_WIDTH, BRAND_HEIGHT, LV_COLOR_FORMAT_RGB565);
    LV_DRAW_BUF_INIT_STATIC(brand_buf);

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(canvas, &brand_buf);
    lv_canvas_fill_bg(canvas, lv_color_hex(COLOR_BG), LV_OPA_COVER);
    for (int y = 0; y < BRAND_HEIGHT; y++) {
        for (int x = 0; x < BRAND_WIDTH; x++) {
            uint8_t bits = BRAND_BITS[y * BRAND_STRIDE + x / 8];
            if (bits & (0x80U >> (x % 8))) {
                lv_canvas_set_px(canvas, x, y, lv_color_hex(COLOR_WHITE), LV_OPA_COVER);
            }
        }
    }
    lv_obj_set_pos(canvas, (240 - BRAND_WIDTH) / 2, 30);
    return canvas;
}

/* ---------- 闲时像素大时钟 ---------- */

#define CLOCK_SCALE     7
#define CLOCK_DIGIT_W   (5 * CLOCK_SCALE)
#define CLOCK_DIGIT_H   (7 * CLOCK_SCALE)
#define CLOCK_COLON_W   CLOCK_SCALE
#define CLOCK_GAP       CLOCK_SCALE
#define CLOCK_TOTAL_W   (4 * CLOCK_DIGIT_W + CLOCK_COLON_W + 4 * CLOCK_GAP)
#define GLYPH_DASH      10

static const uint8_t CLOCK_GLYPHS[11][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F},
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
};

static void clock_draw_digit(lv_layer_t *layer, const lv_area_t *base,
                             int x, int y, int glyph, uint32_t color) {
    for (int gy = 0; gy < 7; gy++) {
        uint8_t bits = CLOCK_GLYPHS[glyph][gy];
        for (int gx = 0; gx < 5; gx++) {
            if (bits & (0x10 >> gx)) {
                draw_pixel_rect(layer, base, x + gx * CLOCK_SCALE,
                                y + gy * CLOCK_SCALE, CLOCK_SCALE, CLOCK_SCALE,
                                color, 0);
            }
        }
    }
}

static void clock_draw_colon(lv_layer_t *layer, const lv_area_t *base,
                             int x, int y, uint32_t color) {
    draw_pixel_rect(layer, base, x, y + 2 * CLOCK_SCALE,
                    CLOCK_COLON_W, CLOCK_COLON_W, color, 0);
    draw_pixel_rect(layer, base, x, y + 4 * CLOCK_SCALE,
                    CLOCK_COLON_W, CLOCK_COLON_W, color, 0);
}

/* ---------- 5x7 像素字体（A-Z，复用时钟数字） ---------- */

static const uint8_t PIX_LETTERS[26][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};
static const uint8_t PIX_BLANK[7] = {0};
static const uint8_t PIX_EXCL[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
static const uint8_t PIX_PCT[7] = {0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13};
static const uint8_t PIX_DOT[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06};

static const uint8_t *pix_glyph(char ch) {
    if (ch >= 'A' && ch <= 'Z') return PIX_LETTERS[ch - 'A'];
    if (ch >= '0' && ch <= '9') return CLOCK_GLYPHS[ch - '0'];
    switch (ch) {
        case ' ': return PIX_BLANK;
        case '-': return CLOCK_GLYPHS[10];
        case '!': return PIX_EXCL;
        case '%': return PIX_PCT;
        case '.': return PIX_DOT;   /* 电压 fallback "3.82V" 需要 */
        default: return PIX_BLANK;
    }
}

static int pix_text_width(const char *text, int scale) {
    int n = (int)strlen(text);
    return n > 0 ? n * 6 * scale - scale : 0;
}

static void pix_text_draw(lv_layer_t *layer, const lv_area_t *base,
                          int x, int y, int scale, const char *text,
                          uint32_t color) {
    for (const char *p = text; *p; p++) {
        const uint8_t *g = pix_glyph(*p);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (g[r] & (0x10 >> c)) {
                    draw_pixel_rect(layer, base, x + c * scale,
                                    y + r * scale, scale, scale, color, 0);
                }
            }
        }
        x += 6 * scale;
    }
}

/* ---------- GO WORK!! 街机 Boss 警告体 ----------
 * 粗体 7 行像素字形（仅 GO WORK!! 所需 9 个 glyph），
 * 右倾斜切 + 硬阴影 + 猛烈抽搐动画。
 * 独立小层（240x56）：咆哮时只重绘底部，不刷整个闲时屏。 */

enum { GW_SCALE = 4, GW_N = 9 };
#define GW_LAYER_Y  244           /* 层在主视图中的 y */
#define GW_LAYER_H  56
#define GW_Y        8             /* 层内局部 y（绝对 252，与旧版静止位一致） */

static const char *GW_ROWS[GW_N][7] = {
    {".####.","##..##","##....","##.###","##..##","##..##",".####."}, /* G */
    {".####.","##..##","##..##","##..##","##..##","##..##",".####."}, /* O */
    {"....","....","....","....","....","....","...."},               /* sp */
    {"##...##","##...##","##...##","##...##","##.#.##","##.#.##","#######"}, /* W */
    {".####.","##..##","##..##","##..##","##..##","##..##",".####."}, /* O */
    {"#####.","##..##","##..##","#####.","##.##.","##..##","##..##"}, /* R */
    {"##..##","##.##.","##.##.","####..","##.##.","##.##.","##..##"}, /* K */
    {"##","##","##","##","##","..","##"},                            /* ! */
    {"##","##","##","##","##","..","##"},                            /* ! */
};
static const uint8_t GW_WIDTH[GW_N] = {6, 6, 4, 7, 6, 6, 6, 2, 2};
/* 右倾：每行向右偏移（像素字形单位） */
static const uint8_t GW_ITAL[7] = {3, 2, 2, 1, 1, 0, 0};

/* 一鞭：240ms / 6 个离散帧，无 easing。前两帧最猛
 * （大幅位移 + CRT 行撕裂 + ghost + 顶行提亮），随后快速衰减归位。
 * 目标是"被狠狠抽了一鞭"，不是持续摇晃。 */
enum { GW_FRAMES = 6, GW_FRAME_MS = 40, GW_FRAME_REST = 255 };
#define GW_BURST_MS (GW_FRAMES * GW_FRAME_MS)
static const int8_t GW_DX[GW_FRAMES] = { 5, -6,  4, -3, 2, 0 };
static const int8_t GW_DY[GW_FRAMES] = {-2,  2, -1,  1, 0, 0 };
/* CRT 行撕裂：最强两帧 顶(0-2)/中(3-4)/底(5-6) 三段水平错位（屏幕像素） */
static const int8_t GW_TEAR_A[3] = {  6, -6,  3 };
static const int8_t GW_TEAR_B[3] = { -5,  7, -3 };

static int gowork_total_w(void) {
    int t = 0;
    for (int i = 0; i < GW_N; i++) t += GW_WIDTH[i] + 1;
    return (t - 1 + GW_ITAL[0]) * GW_SCALE;
}

/* 单层绘制：含静态损伤细节（基线偏移 / 顶部削角 / 行错位）。
 * tear 非空时顶/中/底三段附加水平错位。 */
static void gowork_pass(lv_layer_t *layer, const lv_area_t *base,
                        int dx, int dy, const int8_t *tear,
                        uint32_t top_col, uint32_t body_col) {
    int gx = (240 - gowork_total_w()) / 2 + dx;
    for (int i = 0; i < GW_N; i++) {
        const char *const *rows = GW_ROWS[i];
        int w = GW_WIDTH[i];
        int gdy = (i == 4) ? 2 : 0;                     /* WORK 的 O 下沉 */
        for (int r = 0; r < 7; r++) {
            int rdx = (i == 8 && r == 2) ? GW_SCALE : 0; /* 第二个 ! 错位 */
            bool clip = (i == 7 && r == 0);              /* 第一个 ! 削角 */
            int row_tear = tear ? tear[r < 3 ? 0 : (r < 5 ? 1 : 2)] : 0;
            uint32_t col = (r == 0) ? top_col : body_col;
            for (int c = 0; c < w; c++) {
                if (rows[r][c] != '#') continue;
                if (clip && c > 0) continue;
                draw_pixel_rect(layer, base,
                                gx + c * GW_SCALE + rdx + GW_ITAL[r] * GW_SCALE
                                    + row_tear,
                                GW_Y + r * GW_SCALE + dy + gdy,
                                GW_SCALE, GW_SCALE, col, 0);
            }
        }
        gx += (w + 1) * GW_SCALE;
    }
}

static void gowork_draw(lv_layer_t *layer, const lv_area_t *base) {
    int f = s_gw.frame;
    int dx = (f < GW_FRAMES) ? GW_DX[f] : 0;
    int dy = (f < GW_FRAMES) ? GW_DY[f] : 0;
    const int8_t *tear = (f == 0) ? GW_TEAR_A : (f == 1) ? GW_TEAR_B : NULL;

    /* 硬阴影：glitch 帧与主字横向拉开更远 */
    int sdx = dx + 3, sdy = dy + 3;
    if (f == 0) sdx -= 4;
    else if (f == 1) sdx += 4;
    gowork_pass(layer, base, sdx, sdy, tear, COLOR_RED_DEAD, COLOR_RED_DEAD);

    /* red ghost：仅最强两帧，±8px 撕出的残影 */
    if (f == 0) {
        gowork_pass(layer, base, dx - 8, dy, tear,
                    COLOR_RED_DARK, COLOR_RED_DARK);
    } else if (f == 1) {
        gowork_pass(layer, base, dx + 8, dy + 1, tear,
                    COLOR_RED_DARK, COLOR_RED_DARK);
    }

    /* 主字：顶行橙红高光；最强冲击帧顶行瞬时提亮近白（仅一帧） */
    uint32_t top = (f == 0) ? 0xFFC4B4 : 0xFF6B47;
    gowork_pass(layer, base, dx, dy, tear, top, COLOR_RED);
}

static void gowork_draw_cb(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t base;
    lv_obj_get_coords(obj, &base);
    gowork_draw(layer, &base);
}

/* 动画调度：平时完全静止；每 2s 猛抽一鞭 240ms / 6 个离散帧（无 easing），
 * 咆哮窗口内 timer 临时切 40ms 帧率。只失效 GO WORK 小层。 */
static void gowork_anim_update(uint64_t now_ms) {
    if (!s_timer) return;

    bool scene_ok = !s_stats_view && s_gowork_layer &&
                    s_model.state == POMODORO_IDLE && !s_wifi_banner_on;
    if (s_gw_active && !scene_ok) {
        /* 场景切走：立即终止抽搐并恢复正常帧率 */
        s_gw_active = false;
        s_gw.frame = GW_FRAME_REST;
        lv_timer_set_period(s_timer, 200);
        return;
    }
    if (!scene_ok) return;

    if (now_ms >= s_gw_next_ms) {
        s_gw_next_ms = now_ms + 2000;
        s_gw_end_ms = now_ms + GW_BURST_MS;
        s_gw_active = true;
        lv_timer_set_period(s_timer, GW_FRAME_MS);
    }

    if (s_gw_active) {
        if (now_ms >= s_gw_end_ms) {
            s_gw_active = false;
            s_gw.frame = GW_FRAME_REST;
            lv_timer_set_period(s_timer, 200);
        } else {
            uint32_t f = (uint32_t)(now_ms - (s_gw_end_ms - GW_BURST_MS)) /
                         GW_FRAME_MS;
            if (f >= GW_FRAMES) f = GW_FRAMES - 1;
            s_gw.frame = (uint8_t)f;
        }
        lv_obj_invalidate(s_gowork_layer);
    }
}


static void draw_corner_brackets(lv_layer_t *layer, const lv_area_t *base,
                                 int x1, int y1, int x2, int y2);
static void heart_draw_pixels(lv_layer_t *layer, const lv_area_t *base,
                              int x, int y, int scale, int fill_rows,
                              uint32_t fill_color, uint32_t outline_color);

static void clock_draw_brackets(lv_layer_t *layer, const lv_area_t *base) {
    draw_corner_brackets(layer, base, 22, 72, 218, 133);
}

static void idle_clock_draw_cb(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t base;
    lv_obj_get_coords(obj, &base);

    /* 顶栏：日期 + 电量（像素字体） */
    if (s_idle_date[0]) {
        pix_text_draw(layer, &base, 9, 5, 2, s_idle_date, COLOR_DIM);
    }
    {
        char buf[24];
        battery_text(buf, sizeof(buf));
        if (s_model.muted) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " M");
        }
        int w = pix_text_width(buf, 2);
        pix_text_draw(layer, &base, 213 - w, 5, 2, buf, COLOR_DIM);
    }

    clock_draw_brackets(layer, &base);

    int x = (240 - CLOCK_TOTAL_W) / 2;
    int y = 78;
    uint32_t color = s_clock_valid ? COLOR_WHITE : COLOR_TRACK;
    int glyphs[4];
    if (s_clock_valid) {
        glyphs[0] = s_clock_hh / 10;
        glyphs[1] = s_clock_hh % 10;
        glyphs[2] = s_clock_mm / 10;
        glyphs[3] = s_clock_mm % 10;
    } else {
        glyphs[0] = glyphs[1] = glyphs[2] = glyphs[3] = GLYPH_DASH;
    }

    for (int i = 0; i < 4; i++) {
        clock_draw_digit(layer, &base, x, y, glyphs[i], color);
        x += CLOCK_DIGIT_W + CLOCK_GAP;
        if (i == 1) {
            if (!s_clock_valid) {
                clock_draw_colon(layer, &base, x, y, COLOR_TRACK);
            } else if (s_colon_on) {
                clock_draw_colon(layer, &base, x, y, COLOR_RED);
            }
            x += CLOCK_COLON_W + CLOCK_GAP;
        }
    }

    /* 未同步：右上角黄点 */
    if (s_clock_valid && pomodoro_time_status() != POMO_TIME_SYNCED) {
        draw_pixel_rect(layer, &base, 208, 98, 8, 8, COLOR_YELLOW, 4);
    }

    /* 今日已吃鞭数：一排小血心（最多 10） */
    int whips = s_idle_whips > 10 ? 10 : s_idle_whips;
    if (whips > 0) {
        enum { S = 2, GAP = 3 };
        int w = whips * 9 * S + (whips - 1) * GAP;
        int x = (240 - w) / 2;
        for (int i = 0; i < whips; i++) {
            heart_draw_pixels(layer, &base, x, 146, S, 9, COLOR_RED,
                              COLOR_RED_DEAD);
            x += 9 * S + GAP;
        }
    }

    /* 今日摘要：数字变大时按 完整@2 -> 紧凑@2 -> 紧凑@1 退化，防横向溢出 */
    {
        char buf[40];
        int scale = pomo_summary_fit(buf, sizeof(buf), s_idle_data_ok,
                                     s_idle_whips, s_idle_min, 230);
        pix_text_draw(layer, &base,
                      (240 - pix_text_width(buf, scale)) / 2, 176,
                      scale, buf, COLOR_DIM);
    }

    /* 当前档位 */
    {
        char buf[20];
        snprintf(buf, sizeof(buf), "FOCUS %u MIN",
                 (unsigned)pomodoro_model_focus_min(&s_model));
        pix_text_draw(layer, &base, (240 - pix_text_width(buf, 3)) / 2,
                      204, 3, buf, COLOR_RED);
    }
    /* GO WORK!! 由独立小层 s_gowork_layer 绘制（局部刷新） */
}

/* ---------- 以撒风血心 ---------- */

static const char *HEART_ROWS[9] = {
    ".XX...XX.",
    "XXXX.XXXX",
    "XXXXXXXXX",
    "XXXXXXXXX",
    "XXXXXXXXX",
    ".XXXXXXX.",
    "..XXXXX..",
    "...XXX...",
    "....X....",
};

/* fill_rows: 从底部起实心的行数（0=只剩轮廓） */
static void heart_draw_pixels(lv_layer_t *layer, const lv_area_t *base,
                              int x, int y, int scale, int fill_rows,
                              uint32_t fill_color, uint32_t outline_color) {
    for (int r = 0; r < 9; r++) {
        const char *row = HEART_ROWS[r];
        for (int c = 0; c < 9; c++) {
            if (row[c] != 'X') continue;
            uint32_t color = (8 - r) < fill_rows ? fill_color : outline_color;
            draw_pixel_rect(layer, base, x + c * scale, y + r * scale,
                            scale, scale, color, 0);
        }
    }
}

static void draw_corner_brackets(lv_layer_t *layer, const lv_area_t *base,
                                 int x1, int y1, int x2, int y2) {
    enum { ARM = 12, TH = 2 };
    draw_pixel_rect(layer, base, x1, y1, ARM, TH, COLOR_DIM, 0);
    draw_pixel_rect(layer, base, x1, y1, TH, ARM, COLOR_DIM, 0);
    draw_pixel_rect(layer, base, x2 - ARM, y1, ARM, TH, COLOR_DIM, 0);
    draw_pixel_rect(layer, base, x2 - TH, y1, TH, ARM, COLOR_DIM, 0);
    draw_pixel_rect(layer, base, x1, y2 - TH, ARM, TH, COLOR_DIM, 0);
    draw_pixel_rect(layer, base, x1, y2 - ARM, TH, ARM, COLOR_DIM, 0);
    draw_pixel_rect(layer, base, x2 - ARM, y2 - TH, ARM, TH, COLOR_DIM, 0);
    draw_pixel_rect(layer, base, x2 - TH, y2 - ARM, TH, ARM, COLOR_DIM, 0);
}

/* 计时屏：大血心（专注放血排空 / 休息回血补满），每秒心跳 */
static void big_heart_draw_cb(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t base;
    lv_obj_get_coords(obj, &base);

    /* 角框（层 y 偏移 40）：绝对 (38,52)-(202,168) */
    draw_corner_brackets(layer, &base, 38, 12, 202, 128);

    enum { SCALE = 12 };
    int hx = (240 - 9 * SCALE) / 2;
    int hy = 16;  /* 绝对 56 */

    uint32_t fill = COLOR_RED;
    if (s_heart_flash_on) {
        fill = 0xFF7A6B;
    } else if (s_heart_beat && !s_heart_stopped) {
        fill = COLOR_RED_BEAT;
    } else if (s_heart_stopped) {
        fill = COLOR_RED_DARK;
    }

    int fill_rows = (int)(s_heart_frac * 9 + 0.5f);
    if (fill_rows > 9) fill_rows = 9;
    heart_draw_pixels(layer, &base, hx, hy, SCALE, fill_rows, fill,
                      COLOR_RED_DEAD);
}

static void battery_draw_cb(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t base;
    lv_obj_get_coords(obj, &base);
    draw_pixel_rect(layer, &base, 0, 0, 17, 10, COLOR_DIM, 2);
    draw_pixel_rect(layer, &base, 17, 3, 3, 4, COLOR_DIM, 0);
    draw_pixel_rect(layer, &base, 2, 2, 13, 6, COLOR_BG, 0);
    int fill = 0;
    uint32_t color = COLOR_GREEN;
    if (s_battery_soc >= 0) {
        fill = (s_battery_soc * 11 + 99) / 100;
    } else if (s_battery_state == BATT_FALLBACK &&
               s_battery_mv >= 2500 && s_battery_mv <= 4500) {
        /* 电压粗估 4 档：只是图形等级（黄色示意），不是伪造百分比 */
        color = COLOR_YELLOW;
        fill = s_battery_mv >= 4050 ? 11
             : s_battery_mv >= 3800 ? 8
             : s_battery_mv >= 3600 ? 5
             : s_battery_mv >= 3400 ? 2 : 0;
    }
    if (fill > 0) draw_pixel_rect(layer, &base, 3, 3, fill, 4, color, 0);
}

static void action_draw_cb(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t base;
    lv_obj_get_coords(obj, &base);
    draw_pixel_rect(layer, &base, 0, 1, 3, 14, COLOR_RED, 0);
    draw_pixel_rect(layer, &base, 3, 3, 3, 10, COLOR_RED, 0);
    draw_pixel_rect(layer, &base, 6, 5, 3, 6, COLOR_RED, 0);
    draw_pixel_rect(layer, &base, 9, 7, 2, 2, COLOR_RED, 0);
}

/* 近 7 天柱状图（纵轴分钟） */
static void chart_draw_cb(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t base;
    lv_obj_get_coords(obj, &base);

    int max_min = 1;
    for (int i = 0; i < 7; i++) {
        if (s_chart_min[i] > max_min) max_min = s_chart_min[i];
    }
    for (int i = 0; i < 7; i++) {
        int minutes = s_chart_min[i];
        int h = minutes == 0 ? 3 : 4 + minutes * 76 / max_min;
        uint32_t color = minutes == 0 ? COLOR_TRACK
                         : (i == 6 ? COLOR_RED : COLOR_WHITE);
        draw_pixel_rect(layer, &base, 6 + i * 27, 90 - h, 15, h, color, 1);
    }
    draw_pixel_rect(layer, &base, 0, 92, 190, 2, COLOR_TRACK, 0);
}

static void audio_write_note(int frequency, int duration_ms) {
    int16_t samples[TONE_CHUNK];
    int total = TONE_SAMPLE_RATE * duration_ms / 1000;
    int period = frequency > 0 ? TONE_SAMPLE_RATE / frequency : 1;
    int phase = 0;
    while (total > 0 && !s_audio_cancel) {
        int count = total < TONE_CHUNK ? total : TONE_CHUNK;
        for (int i = 0; i < count; i++) {
            samples[i] = frequency == 0 ? 0 : (phase < period / 2 ? 4200 : -4200);
            if (++phase >= period) phase = 0;
        }
        bsp_audio_write(samples, (size_t)count * sizeof(samples[0]));
        total -= count;
    }
}

static void audio_task(void *arg) {
    (void)arg;
    uint8_t tone;
    bsp_audio_set_format(TONE_SAMPLE_RATE, 16, 1);
    bsp_audio_set_volume(55);
    while (true) {
        if (xQueueReceive(s_audio_queue, &tone, portMAX_DELAY) != pdTRUE) continue;
        if (tone == TONE_START) {
            audio_write_note(880, 90);
            audio_write_note(1175, 110);
        } else if (tone == TONE_PAUSE) {
            audio_write_note(660, 100);
        } else if (tone == TONE_TRIPLE) {
            for (int round = 0; round < 3; round++) {
                audio_write_note(784, 120);
                audio_write_note(988, 120);
                audio_write_note(1319, 200);
            }
        }
    }
}

static void play_tone(tone_id_t tone) {
    if (!s_audio_ok || s_model.muted || !s_audio_queue) return;
    s_audio_cancel = false;
    uint8_t value = tone;
    xQueueOverwrite(s_audio_queue, &value);
}

/* 进入"快速观察"阶段：重置纯逻辑状态机，10s 窗口内 250ms 轮询 */
static void battery_enter_wait_ready(uint64_t now) {
    bg_init(&s_bg);
    s_battery_state = BATT_WAIT_READY;
    s_battery_soc = -1;
    s_batt_deadline_ms = now + BATT_READY_WINDOW_MS;
    s_batt_next_poll_ms = now + BATT_FAST_POLL_MS;
    s_batt_logged_first = false;
}

void demo_pomodoro_prepare(bool audio_ok, bool battery_ok) {
    if (s_prepared) return;
    s_prepared = true;
    s_audio_ok = audio_ok;
    pomodoro_model_defaults(&s_model);
    pomodoro_store_init(&s_model, &s_stats, &s_anchor_unix);
    pomodoro_time_init(s_anchor_unix);

    /* 电池状态机启动：在线则进入 10s 快速观察（连续 3 可信样本才 READY）；
     * 0x63 无应答按 1s/3s/10s 后转 60s 周期重试 init（见 battery_poll）。 */
    if (battery_ok) {
        battery_enter_wait_ready(now_ms());
    } else {
        s_battery_state = BATT_UNAVAILABLE;
        s_batt_retry_count = 0;
        s_batt_retry_ms = now_ms() + 1000;
        ESP_LOGW(TAG, "Battery init failed; retry at 1s/3s/10s then 60s");
    }

    if (s_audio_ok) {
        s_audio_queue = xQueueCreate(1, sizeof(uint8_t));
        if (!s_audio_queue ||
            xTaskCreate(audio_task, "pomo_audio", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "Audio feedback disabled: worker creation failed");
            s_audio_ok = false;
        }
    }
}

/* ---------- 背光与息屏（F5） ---------- */

static bl_scene_t bl_scene_from_state(void) {
    if (s_stats_view) return BL_SCENE_STATS;
    switch (s_model.state) {
        case POMODORO_IDLE:          return BL_SCENE_IDLE;
        case POMODORO_FOCUS_RUNNING: return BL_SCENE_FOCUS;
        case POMODORO_FOCUS_PAUSED:  return BL_SCENE_FOCUS_PAUSED;
        case POMODORO_ABANDON_CONFIRM: return BL_SCENE_CONFIRM;
        case POMODORO_REWARD:        return BL_SCENE_REWARD;
        case POMODORO_BREAK_PROMPT:  return BL_SCENE_BREAK_PROMPT;
        case POMODORO_BREAK_RUNNING: return BL_SCENE_BREAK;
        case POMODORO_BREAK_PAUSED:  return BL_SCENE_BREAK_PAUSED;
    }
    return BL_SCENE_IDLE;
}

static void backlight_apply(uint64_t now) {
    uint64_t elapsed = now - s_bl_ref_ms;
    int target;
    switch (s_bl_scene) {
        case BL_SCENE_IDLE:
            target = elapsed < 15000 ? 100 : (elapsed < 120000 ? 10 : 0);
            break;
        case BL_SCENE_FOCUS:
            target = elapsed < 300000 ? 100 : 50;
            break;
        case BL_SCENE_FOCUS_PAUSED:
            target = elapsed < 60000 ? 100 : 10;
            break;
        case BL_SCENE_BREAK_PROMPT:
            target = elapsed < 15000 ? 100 : (elapsed < 120000 ? 10 : 0);
            break;
        case BL_SCENE_BREAK:
            target = elapsed < 10000 ? 100 : (elapsed < 30000 ? 10 : 0);
            break;
        case BL_SCENE_BREAK_PAUSED:
            target = elapsed < 60000 ? 100 : 10;
            break;
        default:  /* 确认/结算/统计页保持全亮 */
            target = 100;
            break;
    }
    if (target != s_bl_current) {
        s_bl_current = target;
        s_screen_off = target == 0;
        bsp_display_backlight((uint8_t)target);
    }
}

static void bl_on_tick(uint64_t now) {
    bl_scene_t scene = bl_scene_from_state();
    if (scene != s_bl_scene) {
        s_bl_scene = scene;
        s_bl_ref_ms = now;  /* 场景切换重置计时 */
    }
    backlight_apply(now);
}

/* ---------- 刷新 ---------- */

static uint32_t displayed_seconds(void) {
    if (s_model.state == POMODORO_BREAK_RUNNING ||
        s_model.state == POMODORO_BREAK_PAUSED ||
        s_model.state == POMODORO_BREAK_PROMPT) {
        return s_model.state == POMODORO_BREAK_PROMPT
                   ? s_model.pending_break_min * 60U
                   : s_model.break_remaining_sec;
    }
    return s_model.state == POMODORO_IDLE
               ? pomodoro_model_focus_min(&s_model) * 60U
               : s_model.remaining_sec;
}

static void persist(void) {
    int64_t now = pomodoro_time_now_unix();
    if (now > 0) s_anchor_unix = now;
    pomodoro_store_request_save(&s_model, &s_stats, s_anchor_unix);
}

static void refresh_idle_clock(void) {
    int64_t now = pomodoro_time_now_unix();
    if (now <= 0) {
        s_clock_valid = false;
        s_clock_hh = 0;
        s_clock_mm = 0;
        s_idle_date[0] = '\0';
    } else {
        s_clock_valid = true;
        int64_t local = now + POMO_TZ_OFFSET_SEC;
        int sec_of_day = (int)(local % 86400);
        s_clock_hh = sec_of_day / 3600;
        s_clock_mm = (sec_of_day % 3600) / 60;

        uint16_t days = pomo_date_from_unix(now);
        int year, month, day;
        pomo_date_to_ymd(days, &year, &month, &day);
        snprintf(s_idle_date, sizeof(s_idle_date), "%s %02d-%02d",
                 WEEKDAY_NAMES[pomo_weekday(days)], month, day);
    }
    if (s_clock_layer) lv_obj_invalidate(s_clock_layer);
}

static void refresh_idle_summary(void) {
    uint16_t today = pomodoro_time_today();
    if (today == POMO_NO_DATE) {
        s_idle_whips = 0;
        s_idle_min = 0;
        s_idle_data_ok = false;
    } else {
        uint16_t pomos = 0, minutes = 0;
        pomo_stats_day(&s_stats, today, &pomos, &minutes);
        s_idle_whips = pomos;
        s_idle_min = minutes;
        s_idle_data_ok = true;
    }
}

static void refresh_stats_view(void) {
    uint16_t today = pomodoro_time_today();

    if (today == POMO_NO_DATE) {
        lv_label_set_text(s_stats_today, "TODAY -- WHIPS -- MIN");
        lv_label_set_text(s_stats_week, "WEEK -- WHIPS -- MIN");
    } else {
        uint16_t pomos = 0, minutes = 0;
        pomo_stats_day(&s_stats, today, &pomos, &minutes);
        lv_label_set_text_fmt(s_stats_today, "TODAY %u WHIPS %u MIN",
                              pomos, minutes);
        uint32_t week_pomos = 0, week_min = 0;
        pomo_stats_week(&s_stats, today, &week_pomos, &week_min);
        lv_label_set_text_fmt(s_stats_week, "WEEK %u WHIPS %u MIN",
                              (unsigned)week_pomos, (unsigned)week_min);
    }
    lv_label_set_text_fmt(s_stats_all, "ALL %u WHIPS %u MIN",
                          (unsigned)s_stats.total_pomos,
                          (unsigned)s_stats.total_focus_min);

    pomo_day_rec_t week[7];
    pomo_stats_last7(&s_stats, today, week);
    for (int i = 0; i < 7; i++) s_chart_min[i] = week[i].focus_min;

    /* 柱序 today-6..today 对应的星期字母（随实际日期滚动，非固定 MTWTFSS） */
    char letters[7];
    if (today != POMO_NO_DATE) {
        pomo_weekday_letters(today, letters);
        for (int i = 0; i < 7; i++) {
            if (s_stats_weekday[i]) {
                lv_label_set_text_fmt(s_stats_weekday[i], "%c", letters[i]);
            }
        }
    }
    if (s_chart_layer) lv_obj_invalidate(s_chart_layer);
}

/* ---------- WiFi 配网横幅 ---------- */

/* 闲时覆盖层：配网会话期间显示 AP 信息；保存后 15 分钟内显示连接结果。
 * OK 可随时关掉横幅离线继续用番茄钟；AP 5 分钟无操作自动关闭。 */
static void wifi_banner_refresh(void) {
    if (!s_wifi_layer) return;

    /* 配网状态机变化（新会话/保存完成/超时）时清掉旧的 dismiss */
    pomo_prov_state_t ps = pomo_wifi_prov_state();
    if (ps != s_prev_prov_state) {
        s_prev_prov_state = ps;
        s_wifi_banner_dismissed = false;
    }

    bool on = !s_stats_view && s_model.state == POMODORO_IDLE &&
              pomo_wifi_prov_show_banner() && !s_wifi_banner_dismissed;
    s_wifi_banner_on = on;
    set_hidden(s_wifi_layer, !on);
    if (!on) return;

    if (ps == POMO_PROV_ACTIVE) {
        lv_label_set_text(s_wifi_title, "WIFI SETUP");
        lv_obj_set_style_text_color(s_wifi_title, lv_color_hex(COLOR_RED), 0);
        lv_label_set_text(s_wifi_l1, "CONNECT:");
        lv_obj_set_style_text_color(s_wifi_l1, lv_color_hex(COLOR_DIM), 0);
        lv_label_set_text(s_wifi_l2, pomo_wifi_prov_ap_ssid());
        lv_obj_set_style_text_color(s_wifi_l2, lv_color_hex(COLOR_WHITE), 0);
        lv_label_set_text(s_wifi_l3, "OPEN:");
        lv_obj_set_style_text_color(s_wifi_l3, lv_color_hex(COLOR_DIM), 0);
        lv_label_set_text(s_wifi_l4, "192.168.4.1");
        lv_obj_set_style_text_color(s_wifi_l4, lv_color_hex(COLOR_WHITE), 0);
        lv_label_set_text(s_wifi_l5, "OK: OFFLINE");
        lv_obj_set_style_text_color(s_wifi_l5, lv_color_hex(COLOR_DIM), 0);
        return;
    }

    /* 保存后的结果横幅：呈现连接进展 */
    switch (pomodoro_time_wifi_state()) {
        case POMO_WIFI_STATE_CONNECTED:
            lv_label_set_text(s_wifi_title, "WIFI OK");
            lv_obj_set_style_text_color(s_wifi_title,
                                        lv_color_hex(COLOR_WHITE), 0);
            lv_label_set_text(s_wifi_l1, "TIME SYNCED");
            lv_obj_set_style_text_color(s_wifi_l1,
                                        lv_color_hex(COLOR_WHITE), 0);
            break;
        case POMO_WIFI_STATE_FAILED:
            lv_label_set_text(s_wifi_title, "WIFI FAILED");
            lv_obj_set_style_text_color(s_wifi_title,
                                        lv_color_hex(COLOR_RED), 0);
            lv_label_set_text(s_wifi_l1, "HOLD DOWN FOR SETUP");
            lv_obj_set_style_text_color(s_wifi_l1,
                                        lv_color_hex(COLOR_WHITE), 0);
            break;
        default:
            lv_label_set_text(s_wifi_title, "WIFI");
            lv_obj_set_style_text_color(s_wifi_title,
                                        lv_color_hex(COLOR_WHITE), 0);
            lv_label_set_text(s_wifi_l1, "CONNECTING...");
            lv_obj_set_style_text_color(s_wifi_l1,
                                        lv_color_hex(COLOR_DIM), 0);
            break;
    }
    lv_label_set_text(s_wifi_l2, "");
    lv_label_set_text(s_wifi_l3, "");
    lv_label_set_text(s_wifi_l4, "");
    lv_label_set_text(s_wifi_l5, "");
}

static void refresh_ui(void) {
    if (!s_scr) return;
    wifi_banner_refresh();
    if (s_stats_view) {
        refresh_stats_view();
        return;
    }

    bool idle = s_model.state == POMODORO_IDLE;
    bool reward = s_model.state == POMODORO_REWARD;
    bool timing = !idle && !reward;

    if (!idle) {
        lv_label_set_text_fmt(s_top_label, "WHIP %u/4",
                              s_model.pomodoro_round + 1);
    }

    {
        char buf[24];
        battery_text(buf, sizeof(buf));
        if (s_model.muted) {
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " M");
        }
        lv_label_set_text(s_battery_label, buf);
    }
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -27, 5);
    if (s_battery_icon) lv_obj_invalidate(s_battery_icon);

    set_hidden(s_brand, !idle);
    set_hidden(s_clock_layer, !idle);
    set_hidden(s_gowork_layer, !idle);
    set_hidden(s_top_label, idle);      /* 闲时日期/电量由像素层绘制 */
    set_hidden(s_battery_label, idle);
    if (idle) {
        refresh_idle_clock();
        refresh_idle_summary();
    }

    set_hidden(s_heart_layer, idle);
    set_hidden(s_mmss_label, !timing);
    set_hidden(s_plus_label, !reward);
    set_hidden(s_done_label, !reward);
    set_hidden(s_state_label, !timing);
    set_hidden(s_action_icon, !timing);

    if (timing) {
        uint32_t seconds = displayed_seconds();
        uint32_t total = pomodoro_model_focus_min(&s_model) * 60U;
        bool is_break = s_model.state == POMODORO_BREAK_PROMPT ||
                        s_model.state == POMODORO_BREAK_RUNNING ||
                        s_model.state == POMODORO_BREAK_PAUSED;
        if (is_break) total = s_model.pending_break_min * 60U;

        s_heart_stopped = s_model.state == POMODORO_FOCUS_PAUSED ||
                          s_model.state == POMODORO_ABANDON_CONFIRM ||
                          s_model.state == POMODORO_BREAK_PAUSED;
        s_heart_flash_on = false;
        s_heart_frac = total ? (float)seconds / (float)total : 0.0f;
        if (is_break) s_heart_frac = 1.0f - s_heart_frac;  /* 休息=回血 */
        lv_label_set_text_fmt(s_mmss_label, "%02lu:%02lu",
                              (unsigned long)(seconds / 60),
                              (unsigned long)(seconds % 60));

        switch (s_model.state) {
            case POMODORO_FOCUS_RUNNING:
                lv_label_set_text(s_state_label, "PAUSE");
                break;
            case POMODORO_FOCUS_PAUSED:
                lv_label_set_text(s_state_label, "RESUME");
                break;
            case POMODORO_ABANDON_CONFIRM:
                lv_label_set_text(s_state_label, "ABANDON?");
                break;
            case POMODORO_BREAK_PROMPT:
                lv_label_set_text(s_state_label, "REST");
                break;
            case POMODORO_BREAK_RUNNING:
                lv_label_set_text(s_state_label, "PAUSE");
                break;
            case POMODORO_BREAK_PAUSED:
                lv_label_set_text(s_state_label, "RESUME");
                break;
            default:
                break;
        }
    } else if (reward) {
        s_heart_stopped = false;
        s_heart_frac = 1.0f;
    }
    if (s_heart_layer) lv_obj_invalidate(s_heart_layer);
}

static void set_stats_view(bool enabled) {
    s_stats_view = enabled;
    s_stats_open_ms = now_ms();
    set_hidden(s_stats_layer, !enabled);
    set_hidden(s_main_layer, enabled);
    refresh_ui();
}

/* ---------- 定时器 ---------- */

/* 电量计 init 重试节奏：1s / 3s / 10s 之后每 60s 一次 */
static void battery_schedule_retry(uint64_t now) {
    static const uint32_t EARLY_MS[3] = { 1000, 3000, 10000 };
    uint32_t delay = s_batt_retry_count < 3 ? EARLY_MS[s_batt_retry_count]
                                            : 60ULL * 1000;
    s_batt_retry_count++;
    s_batt_retry_ms = now + delay;
}

/* 采样一次 SOC raw + VCELL，喂给纯逻辑判定；电压做 EMA 平滑。 */
static void batt_sample(void) {
    uint16_t raw = 0;
    bool ok = bsp_battery_soc_raw(&raw);
    int mv = bsp_battery_mv();
    if (mv > 0) {
        s_battery_mv = (s_battery_mv < 0) ? mv : (s_battery_mv * 3 + mv) / 4;
        s_batt_mv_fail_streak = 0;
    } else if (++s_batt_mv_fail_streak >= 3) {
        s_battery_mv = -1;   /* I2C 持续失败：清掉冻结的电压显示，回到 --% */
    }

    unsigned x1000 = ((raw >> 8) & 0xFF) * 1000U +
                     (unsigned)(((raw & 0xFF) * 1000U + 128U) / 256U);
    bool plausible = bg_sample_plausible(ok, raw, mv);
    bg_state_t prev = s_bg.state;
    bool changed = bg_feed(&s_bg, ok, raw, mv);

    if (!s_batt_logged_first) {
        /* 每轮 WAIT_READY 只打一次首样本详情，避免高频刷屏 */
        s_batt_logged_first = true;
        const char *reason = !ok ? "i2c-fail"
                            : !plausible ? ((((raw >> 8) & 0xFF) > 100)
                                             ? "soc-invalid" : "startup-zero")
                                          : "candidate";
        ESP_LOGI(TAG, "BATTERY raw=0x%04X soc=%u.%03u%% v=%dmV state=WAIT reason=%s",
                 raw, x1000 / 1000, x1000 % 1000, mv, reason);
    } else if (plausible && prev != BG_READY) {
        if (s_bg.state == BG_READY) {
            ESP_LOGI(TAG, "BATTERY raw=0x%04X soc=%u.%03u%% v=%dmV valid=%d/%d -> READY",
                     raw, x1000 / 1000, x1000 % 1000, mv,
                     BG_READY_STREAK, BG_READY_STREAK);
        } else {
            ESP_LOGI(TAG, "BATTERY raw=0x%04X soc=%u.%03u%% v=%dmV valid=%u/%d",
                     raw, x1000 / 1000, x1000 % 1000, mv,
                     s_bg.valid_streak, BG_READY_STREAK);
        }
    } else if (changed && s_bg.state == BG_FALLBACK) {
        ESP_LOGW(TAG, "BATTERY raw=0x%04X v=%dmV -> FALLBACK reason=invalid-streak",
                 raw, mv);
    }

    if (changed) {
        s_battery_soc = bg_display_soc(&s_bg);
        refresh_ui();
    }
}

static batt_state_t batt_state_from_bg(void) {
    switch (s_bg.state) {
    case BG_READY:    return BATT_READY;
    case BG_FALLBACK: return BATT_FALLBACK;
    default:          return BATT_WAIT_READY;
    }
}

/* FALLBACK 态电压文案节流：平滑值变化 ≥100mV 立即刷，
 * 否则最快 30s 一次。避免 4.08→4.12 级别的抖动重绘。 */
static void batt_volt_ui_update(uint64_t now) {
    if (s_battery_state != BATT_FALLBACK || s_battery_mv < 0) return;
    int diff = s_battery_mv - s_batt_volt_shown;
    if (diff < 0) diff = -diff;
    if (s_batt_volt_shown >= 0 && diff < 100 &&
        now - s_batt_volt_ui_ms < 30ULL * 1000) return;
    s_batt_volt_shown = s_battery_mv;
    s_batt_volt_ui_ms = now;
    refresh_ui();
}

/* 有证据的 gauge 恢复：仅当 CONFIG(0x08) 偏离正常模式 0x00（如重回睡眠 0xF0
 * 或挂起 0x30）才 restart；SOC 长期为 0x0000 不构成证据——无脑 restart 只会让
 * 芯片反复回到 startup 默认值。限额 2 次/boot，60s cooldown。 */
static void batt_maybe_restart(uint64_t now) {
    if (s_batt_restart_count >= BATT_MAX_RESTARTS) return;
    if (s_batt_recovery_ms != 0 &&
        now - s_batt_recovery_ms < BATT_RESTART_COOLDOWN_MS) return;

    bsp_battery_diag_t d;
    if (!bsp_battery_diag(&d)) return;
    if (d.config == 0x00) return;

    ESP_LOGW(TAG, "BATTERY CONFIG=0x%02X 非正常模式，recovery restart %u/%d",
             d.config, s_batt_restart_count + 1, BATT_MAX_RESTARTS);
    if (bsp_battery_restart() == ESP_OK) {
        s_batt_restart_count++;
        s_batt_recovery_ms = now;
        battery_enter_wait_ready(now);
    }
}

/* 进入 FALLBACK：记录当前平滑电压为已显示值（首帧立即生效） */
static void batt_fallback_begin(uint64_t now) {
    s_batt_volt_shown = s_battery_mv;
    s_batt_volt_ui_ms = now;
    s_batt_next_poll_ms = now + BATT_FALLBACK_POLL_MS;
}

/* 电池状态机轮询（基于真实时间；GO WORK 动画会临时切 40ms 帧率）。
 * UNAVAILABLE: 重试 init；WAIT_READY: 250ms×10s 观察，连续 3 可信 → READY；
 * READY: 30s 轮询，连续 3 异常 → FALLBACK；
 * FALLBACK: 显示平滑电压，5s 低频读 SOC（连续 3 可信才回 READY），
 *           仅 CONFIG 异常等有证据时 restart（≤2 次/boot）。 */
static void battery_poll(uint64_t now) {
    switch (s_battery_state) {
    case BATT_UNAVAILABLE:
        if (now >= s_batt_retry_ms) {
            battery_schedule_retry(now);
            if (bsp_battery_init() == ESP_OK) battery_enter_wait_ready(now);
        }
        break;

    case BATT_WAIT_READY:
        if (now < s_batt_next_poll_ms) break;
        s_batt_next_poll_ms = now + BATT_FAST_POLL_MS;
        batt_sample();
        s_battery_state = batt_state_from_bg();
        if (s_battery_state == BATT_READY) {
            s_batt_next_poll_ms = now + BATT_READY_POLL_MS;
        } else if (now >= s_batt_deadline_ms) {
            /* 观察窗口耗尽：不判芯片坏，转电压 fallback 后台继续低频等 */
            bg_force_fallback(&s_bg);
            s_battery_state = BATT_FALLBACK;
            batt_fallback_begin(now);
            ESP_LOGW(TAG, "BATTERY state=FALLBACK reason=ready-timeout v=%dmV",
                     s_battery_mv);
            refresh_ui();
        }
        break;

    case BATT_READY:
        if (now < s_batt_next_poll_ms) break;
        s_batt_next_poll_ms = now + BATT_READY_POLL_MS;
        batt_sample();
        s_battery_state = batt_state_from_bg();
        if (s_battery_state == BATT_FALLBACK) {
            batt_fallback_begin(now);
        }
        break;

    case BATT_FALLBACK:
        if (now < s_batt_next_poll_ms) break;
        s_batt_next_poll_ms = now + BATT_FALLBACK_POLL_MS;
        batt_sample();
        s_battery_state = batt_state_from_bg();
        if (s_battery_state == BATT_READY) {
            s_batt_next_poll_ms = now + BATT_READY_POLL_MS;
        } else {
            batt_maybe_restart(now);
            batt_volt_ui_update(now);
        }
        break;
    }
}

static void timer_cb(lv_timer_t *timer) {
    (void)timer;
    uint64_t time_ms = now_ms();

    pomodoro_event_t event = pomodoro_model_tick(&s_model, time_ms);
    if (event == POMODORO_EVENT_FOCUS_COMPLETE) {
        uint16_t today = pomodoro_time_today();
        pomo_stats_record(&s_stats, today,
                          pomodoro_model_focus_min(&s_model));
        play_tone(TONE_TRIPLE);
        persist();
    } else if (event == POMODORO_EVENT_BREAK_COMPLETE) {
        play_tone(TONE_TRIPLE);
        persist();
    } else if (event == POMODORO_EVENT_REWARD_FINISHED ||
               event == POMODORO_EVENT_CONFIRM_TIMEOUT) {
        persist();
    }

    bl_on_tick(time_ms);

    pomo_time_status_t status = pomodoro_time_status();
    if (status != s_last_time_status) {
        s_last_time_status = status;
        if (status == POMO_TIME_SYNCED) persist();  /* 落盘新鲜锚点 */
        refresh_ui();
    }

    uint32_t seconds = displayed_seconds();
    if (seconds != s_last_sec || event != POMODORO_EVENT_NONE) {
        s_last_sec = seconds;
        if (s_model.state == POMODORO_FOCUS_RUNNING ||
            s_model.state == POMODORO_BREAK_RUNNING) {
            s_heart_beat_until = time_ms + 200;
        }
        refresh_ui();
    }

    bool beat = time_ms < s_heart_beat_until;
    if (beat != s_heart_beat) {
        s_heart_beat = beat;
        if (s_heart_layer) lv_obj_invalidate(s_heart_layer);
    }

    /* GO WORK!! 咆哮抽搐动画（调度式，见 gowork_anim_update） */
    gowork_anim_update(time_ms);

    if (s_model.state == POMODORO_IDLE) {
        uint32_t minute = (uint32_t)(pomodoro_time_now_unix() / 60);
        if (minute != s_last_minute) {
            s_last_minute = minute;
            refresh_ui();
        }
        uint32_t half_sec = (uint32_t)(time_ms / 500);
        if (half_sec != s_blink_bucket) {
            s_blink_bucket = half_sec;
            s_colon_on = (half_sec % 2) == 0;
            if (s_clock_layer && !s_stats_view) {
                lv_obj_invalidate(s_clock_layer);
            }
        }
    }

    if (s_model.state == POMODORO_FOCUS_RUNNING ||
        s_model.state == POMODORO_BREAK_RUNNING) {
        uint32_t bucket = seconds / 60;
        if (bucket != s_save_bucket) {
            s_save_bucket = bucket;
            persist();
        }
    }

    if (s_model.state == POMODORO_IDLE &&
        time_ms - s_last_hourly_ms >= 60ULL * 60 * 1000) {
        s_last_hourly_ms = time_ms;
        persist();  /* 闲时小时级锚点刷新 */
    }

    if (s_model.state == POMODORO_REWARD) {
        uint32_t phase = (uint32_t)(time_ms % 800);
        bool on = (phase / 160) % 2 == 0;
        if (on != s_heart_flash_on) {
            s_heart_flash_on = on;
            if (s_heart_layer) lv_obj_invalidate(s_heart_layer);
        }
    }

    if (s_stats_view && time_ms - s_stats_open_ms >= 10000) {
        set_stats_view(false);
    }

    /* WiFi 配网横幅轮询：prov/wifi 状态变化时刷新内容（1s 粒度） */
    if (time_ms - s_last_wifi_check_ms >= 1000) {
        s_last_wifi_check_ms = time_ms;
        uint8_t sig = (uint8_t)(((pomo_wifi_prov_state() & 3) << 4) |
                                ((pomodoro_time_wifi_state() & 3) << 2) |
                                (s_wifi_banner_dismissed ? 2 : 0) |
                                (pomo_wifi_prov_show_banner() ? 1 : 0));
        if (sig != s_wifi_banner_sig) {
            s_wifi_banner_sig = sig;
            refresh_ui();
        }
    }

    /* 电池状态机轮询（见 battery_poll） */
    battery_poll(time_ms);
}

/* ---------- 进出与按键 ---------- */

void demo_pomodoro_enter(void) {
    if (!s_prepared) demo_pomodoro_prepare(false, false);
    s_stats_view = false;
    s_last_sec = UINT32_MAX;
    s_last_minute = UINT32_MAX;
    s_save_bucket = UINT32_MAX;
    s_last_time_status = pomodoro_time_status();
    s_last_hourly_ms = now_ms();
    s_last_wifi_check_ms = now_ms();
    s_wifi_banner_sig = 0xFF;
    s_bl_scene = bl_scene_from_state();
    s_bl_ref_ms = now_ms();
    s_bl_current = 100;
    s_screen_off = false;
    s_gw.frame = GW_FRAME_REST;
    s_gw_active = false;
    s_gw_next_ms = now_ms() + 2000;

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    s_main_layer = container(s_scr, 0, 0, 240, 320);

    /* 顶栏 */
    s_top_label = label(s_main_layer, &lv_font_montserrat_14, COLOR_DIM);
    lv_obj_set_pos(s_top_label, 9, 5);
    s_battery_label = label(s_main_layer, &lv_font_montserrat_14, COLOR_DIM);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -27, 5);
    s_battery_icon = draw_layer_create(s_main_layer, 217, 8, 20, 10,
                                       battery_draw_cb);
    s_brand = brand_create(s_main_layer);

    /* 闲时屏全像素层：日期/电量/时钟/血心行/摘要/档位 */
    s_clock_layer = draw_layer_create(s_main_layer, 0, 0, 240, 320,
                                      idle_clock_draw_cb);

    /* GO WORK!! 独立小层：咆哮时只重绘底部 56px，静止位与旧版一致 */
    s_gowork_layer = draw_layer_create(s_main_layer, 0, GW_LAYER_Y,
                                       240, GW_LAYER_H, gowork_draw_cb);

    /* 计时屏：血心排空 + 下方大号倒计时 */
    s_heart_layer = draw_layer_create(s_main_layer, 0, 40, 240, 150,
                                      big_heart_draw_cb);
    s_mmss_label = centered_label(s_main_layer, &lv_font_montserrat_48,
                                  COLOR_WHITE, 184);
    s_state_label = label(s_main_layer, &lv_font_montserrat_14, COLOR_RED);
    /* 70px 会让 ABANDON? 折行；加宽 + CLIP 保证 PAUSE/RESUME/REST/ABANDON?
     * 统一字号、永远单行（x=42 + 150 = 192 < 240 右边界） */
    lv_obj_set_width(s_state_label, 150);
    lv_label_set_long_mode(s_state_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_state_label, 42, 286);
    s_action_icon = draw_layer_create(s_main_layer, 27, 287, 11, 16,
                                      action_draw_cb);

    /* 结算屏：满心 + WHIP LANDED */
    s_plus_label = centered_label(s_main_layer, &lv_font_montserrat_48,
                                  COLOR_WHITE, 88);
    lv_label_set_text(s_plus_label, "+1");
    s_done_label = centered_label(s_main_layer, &lv_font_montserrat_14,
                                  COLOR_DIM, 196);
    lv_label_set_text(s_done_label, "WHIP LANDED");

    /* WiFi 配网/结果横幅：闲时覆盖层（黑底白字红标题，与整机视觉一致） */
    s_wifi_layer = container(s_main_layer, 0, 0, 240, 320);
    lv_obj_set_style_bg_color(s_wifi_layer, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_wifi_layer, LV_OPA_COVER, 0);
    s_wifi_title = centered_label(s_wifi_layer, &lv_font_montserrat_20,
                                  COLOR_RED, 60);
    s_wifi_l1 = centered_label(s_wifi_layer, &lv_font_montserrat_14,
                               COLOR_DIM, 112);
    s_wifi_l2 = centered_label(s_wifi_layer, &lv_font_montserrat_14,
                               COLOR_WHITE, 134);
    s_wifi_l3 = centered_label(s_wifi_layer, &lv_font_montserrat_14,
                               COLOR_DIM, 158);
    s_wifi_l4 = centered_label(s_wifi_layer, &lv_font_montserrat_14,
                               COLOR_WHITE, 180);
    s_wifi_l5 = centered_label(s_wifi_layer, &lv_font_montserrat_14,
                               COLOR_DIM, 226);
    set_hidden(s_wifi_layer, true);

    /* 统计页 */
    s_stats_layer = container(s_scr, 0, 0, 240, 320);
    lv_obj_t *title = centered_label(s_stats_layer, &lv_font_montserrat_20,
                                     COLOR_WHITE, 31);
    lv_label_set_text(title, "STATISTICS");
    s_stats_today = centered_label(s_stats_layer, &lv_font_montserrat_14,
                                   COLOR_DIM, 64);
    s_chart_layer = draw_layer_create(s_stats_layer, 25, 92, 190, 96,
                                      chart_draw_cb);
    for (int i = 0; i < 7; i++) {
        s_stats_weekday[i] = label(s_stats_layer, &lv_font_montserrat_14,
                                   COLOR_DIM);
        lv_obj_set_pos(s_stats_weekday[i], 31 + i * 27, 194);
        lv_label_set_text(s_stats_weekday[i], "-");  /* 首刷时按日期填充 */
    }
    s_stats_week = centered_label(s_stats_layer, &lv_font_montserrat_14,
                                  COLOR_WHITE, 230);
    s_stats_all = centered_label(s_stats_layer, &lv_font_montserrat_14,
                                 COLOR_WHITE, 254);
    lv_obj_t *hint = centered_label(s_stats_layer, &lv_font_montserrat_14,
                                    COLOR_DIM, 292);
    lv_label_set_text(hint, "OK: BACK");
    set_hidden(s_stats_layer, true);

    refresh_ui();
    s_timer = lv_timer_create(timer_cb, 200, NULL);
    lv_screen_load(s_scr);
}

void demo_pomodoro_exit(void) {
    if (s_model.state == POMODORO_FOCUS_RUNNING ||
        s_model.state == POMODORO_BREAK_RUNNING) {
        pomodoro_model_pause(&s_model, now_ms());
    }
    persist();
    s_audio_cancel = true;
    if (s_audio_queue) xQueueReset(s_audio_queue);
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) lv_obj_delete(s_scr);
    s_scr = s_main_layer = s_stats_layer = NULL;
    s_top_label = s_battery_label = s_battery_icon = s_brand = NULL;
    s_clock_layer = NULL;
    s_gowork_layer = NULL;
    s_heart_layer = NULL;
    s_mmss_label = s_state_label = s_action_icon = NULL;
    s_plus_label = s_done_label = NULL;
    s_wifi_layer = s_wifi_title = NULL;
    s_wifi_l1 = s_wifi_l2 = s_wifi_l3 = NULL;
    s_wifi_l4 = s_wifi_l5 = NULL;
    s_wifi_banner_on = false;
    s_stats_today = s_chart_layer = s_stats_week = s_stats_all = NULL;
    for (int i = 0; i < 7; i++) s_stats_weekday[i] = NULL;
    s_stats_view = false;
    s_screen_off = false;
    s_bl_current = 100;
    bsp_display_backlight(100);
}

void demo_pomodoro_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    uint64_t time_ms = now_ms();

    /* 熄灭态：首键仅唤醒，不触发任何动作 */
    if (s_screen_off) {
        s_bl_ref_ms = time_ms;
        backlight_apply(time_ms);
        return;
    }
    if (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE) {
        s_bl_ref_ms = time_ms;  /* 按键活动刷新计时 */
    }

    if (s_stats_view) {
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE)) {
            set_stats_view(false);
        } else {
            s_stats_open_ms = time_ms;
        }
        return;
    }

    /* 配网横幅：OK 关掉提示，番茄钟离线继续可用（AP 后台仍等 5 分钟） */
    if (s_wifi_banner_on && btn == BSP_BTN_OK &&
        (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE)) {
        s_wifi_banner_dismissed = true;
        refresh_ui();
        return;
    }

    /* IDLE + 长按 DOWN：手动重新进入 WiFi 配网。
     * 不删旧凭据；只有网页提交新的有效 SSID 才覆盖 NVS。
     * （不用组合键：三键共用 ADC 分压不可靠；不用长按 OK：已被全局返回菜单占用） */
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_DOWN &&
        s_model.state == POMODORO_IDLE) {
        pomo_wifi_prov_start();
        s_wifi_banner_dismissed = false;
        refresh_ui();
        return;
    }

    if (ev == BSP_BTN_DOUBLE) {
        if (s_model.state == POMODORO_IDLE && btn == BSP_BTN_UP) {
            set_stats_view(true);
        } else if (s_model.state == POMODORO_IDLE && btn == BSP_BTN_DOWN) {
            s_model.muted = !s_model.muted;
            persist();
            refresh_ui();
        }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;

    bool changed = false;
    switch (s_model.state) {
        case POMODORO_IDLE:
            if (btn == BSP_BTN_OK) {
                changed = pomodoro_model_start_focus(&s_model, time_ms);
                if (changed) play_tone(TONE_START);
            } else if (btn == BSP_BTN_UP) {
                changed = pomodoro_model_select_duration(&s_model, -1);
            } else if (btn == BSP_BTN_DOWN) {
                changed = pomodoro_model_select_duration(&s_model, 1);
            }
            break;
        case POMODORO_FOCUS_RUNNING:
        case POMODORO_BREAK_RUNNING:
            if (btn == BSP_BTN_OK) {
                changed = pomodoro_model_pause(&s_model, time_ms);
                if (changed) play_tone(TONE_PAUSE);
            }
            break;
        case POMODORO_FOCUS_PAUSED:
            if (btn == BSP_BTN_OK) changed = pomodoro_model_resume(&s_model, time_ms);
            else if (btn == BSP_BTN_UP) {
                changed = pomodoro_model_request_abandon(&s_model, time_ms);
            }
            break;
        case POMODORO_ABANDON_CONFIRM:
            if (btn == BSP_BTN_UP) changed = pomodoro_model_confirm_abandon(&s_model);
            else if (btn == BSP_BTN_DOWN || btn == BSP_BTN_OK) {
                changed = pomodoro_model_cancel_abandon(&s_model);
            }
            break;
        case POMODORO_BREAK_PROMPT:
            if (btn == BSP_BTN_OK) changed = pomodoro_model_start_break(&s_model, time_ms);
            else if (btn == BSP_BTN_UP) changed = pomodoro_model_skip_break(&s_model);
            break;
        case POMODORO_BREAK_PAUSED:
            if (btn == BSP_BTN_OK) changed = pomodoro_model_resume(&s_model, time_ms);
            break;
        case POMODORO_REWARD:
            break;
    }

    if (changed) {
        s_last_sec = UINT32_MAX;
        persist();
        refresh_ui();
    }
}
