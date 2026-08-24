#pragma once

#include <stdbool.h>
#include <stddef.h>
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

/* 统计页近 7 天星期首字母：out[0]=today-6 .. out[6]=today，
 * 取值 M/T/W/T/F/S/S，与 pomo_stats_last7 的柱序一一对应。 */
void pomo_weekday_letters(uint16_t today, char out[7]);

/* 闲时今日摘要自适应排版（纯逻辑，主机可测）：
 * 按"完整文案 scale2 -> 紧凑文案 scale2 -> 紧凑文案 scale1"退化，
 * 选用首个宽度 <= max_px 的档位；has_data=false 时用 -- 占位。
 * 写出的文案保证宽度不超 240px 屏的安全区（max_px 建议 230）。
 * 返回所用 scale（1 或 2）。宽度公式须与 demo_pomodoro.c 的
 * pix_text_width 一致：5x7 像素字体，advance = 6*scale，去尾部 gap。 */
int pomo_summary_fit(char *buf, size_t cap, bool has_data,
                     unsigned whips, unsigned minutes, int max_px);

/* 以"锚点时刻 + 流逝开机时长"推算当前 unix 时间；now 早于锚点时刻时钳回锚点。 */
int64_t pomo_time_estimate_unix(int64_t anchor_unix, uint64_t anchor_uptime_ms,
                                uint64_t now_uptime_ms);
