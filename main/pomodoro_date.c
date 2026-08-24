#include "pomodoro_date.h"

#include <stdio.h>
#include <stddef.h>

/* Howard Hinnant 的 civil 日期算法，days 为自 1970-01-01 起的天数。 */
static void civil_from_days(int64_t z, int *year, int *month, int *day) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    uint32_t dd = doy - (153 * mp + 2) / 5 + 1;
    uint32_t mm = mp + (mp < 10 ? 3 : -9);
    *year = (int)(yy + (mm <= 2));
    *month = (int)mm;
    *day = (int)dd;
}

uint16_t pomo_date_from_unix(int64_t unix_sec) {
    int64_t local = unix_sec + POMO_TZ_OFFSET_SEC;
    if (local < 0) local = 0;
    return (uint16_t)(local / 86400);
}

int64_t pomo_unix_at_local_midnight(uint16_t days) {
    return (int64_t)days * 86400 - POMO_TZ_OFFSET_SEC;
}

void pomo_date_to_ymd(uint16_t days, int *year, int *month, int *day) {
    civil_from_days((int64_t)days, year, month, day);
}

uint8_t pomo_weekday(uint16_t days) {
    /* 1970-01-01 是周四：days=0 -> weekday=3(周一计 0)。 */
    return (uint8_t)((days + 3) % 7);
}

uint16_t pomo_week_start(uint16_t days) {
    return (uint16_t)(days - pomo_weekday(days));
}

void pomo_weekday_letters(uint16_t today, char out[7]) {
    static const char L[7] = {'M', 'T', 'W', 'T', 'F', 'S', 'S'};
    for (int i = 0; i < 7; i++) {
        out[i] = L[pomo_weekday((uint16_t)(today - 6 + i))];
    }
}

/* 5x7 像素字体宽度：advance = 6*scale，去尾部 gap。
 * 须与 demo_pomodoro.c 的 pix_text_width 保持一致。 */
static int pix_text_width(int n_chars, int scale) {
    return n_chars > 0 ? n_chars * 6 * scale - scale : 0;
}

int pomo_summary_fit(char *buf, size_t cap, bool has_data,
                     unsigned whips, unsigned minutes, int max_px) {
    char full[40];
    int n;
    if (has_data) {
        n = snprintf(full, sizeof(full), "TODAY %u WHIPS %u MIN", whips, minutes);
    } else {
        n = snprintf(full, sizeof(full), "TODAY -- WHIPS -- MIN");
    }
    if (n < 0) n = 0;
    if (n < (int)sizeof(full) && pix_text_width(n, 2) <= max_px) {
        snprintf(buf, cap, "%s", full);
        return 2;
    }

    /* 完整文案 scale2 放不下：紧凑文案（去掉 TODAY 前缀）。 */
    char comp[40];
    if (has_data) {
        n = snprintf(comp, sizeof(comp), "%u WHIPS %u MIN", whips, minutes);
    } else {
        n = snprintf(comp, sizeof(comp), "-- WHIPS -- MIN");
    }
    if (n < 0) n = 0;
    if (n < (int)sizeof(comp) && pix_text_width(n, 2) <= max_px) {
        snprintf(buf, cap, "%s", comp);
        return 2;
    }

    /* 仍放不下：降为 scale1（uint16 极值 21 字符 = 125px，必然安全）。 */
    snprintf(buf, cap, "%s", comp);
    return 1;
}

int64_t pomo_time_estimate_unix(int64_t anchor_unix, uint64_t anchor_uptime_ms,
                                uint64_t now_uptime_ms) {
    if (anchor_unix <= 0) return 0;
    if (now_uptime_ms <= anchor_uptime_ms) return anchor_unix;
    return anchor_unix + (int64_t)((now_uptime_ms - anchor_uptime_ms) / 1000);
}
