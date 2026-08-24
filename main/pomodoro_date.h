#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 时区固定 UTC+8。无 RTC/NTP 依据时不可用动态时区，中国也不使用夏令时。 */
#define POMO_TZ_OFFSET_SEC (8LL * 3600)

/* unix 秒(UTC) -> 本地时区的 epoch 天数(1970-01-01 为第 0 天)。 */
uint16_t pomo_date_from_unix(int64_t unix_sec);

/* 本地 epoch 天数 -> 该日本地 00:00 对应的 unix 秒。 */
int64_t pomo_unix_at_local_midnight(uint16_t days);

/* 本地 epoch 天数 -> 年月日。 */
void pomo_date_to_ymd(uint16_t days, int *year, int *month, int *day);

/* 星期：0=周一 .. 6=周日。 */
uint8_t pomo_weekday(uint16_t days);

/* 该日所在周的周一。 */
uint16_t pomo_week_start(uint16_t days);

/* 以"锚点时刻 + 流逝开机时长"推算当前 unix 时间；now 早于锚点时刻时钳回锚点。 */
int64_t pomo_time_estimate_unix(int64_t anchor_unix, uint64_t anchor_uptime_ms,
                                uint64_t now_uptime_ms);
