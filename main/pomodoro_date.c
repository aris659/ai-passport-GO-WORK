#include "pomodoro_date.h"

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

int64_t pomo_time_estimate_unix(int64_t anchor_unix, uint64_t anchor_uptime_ms,
                                uint64_t now_uptime_ms) {
    if (anchor_unix <= 0) return 0;
    if (now_uptime_ms <= anchor_uptime_ms) return anchor_unix;
    return anchor_unix + (int64_t)((now_uptime_ms - anchor_uptime_ms) / 1000);
}
