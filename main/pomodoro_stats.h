#pragma once

#include <stdbool.h>
#include <stdint.h>

#define POMO_STATS_DAYS 90
/* 无时间基准时完成只计累计，不落日期桶。 */
#define POMO_NO_DATE 0xFFFF

typedef struct {
    uint16_t date;       /* 本地 epoch 天数 */
    uint16_t pomos;
    uint16_t focus_min;
} pomo_day_rec_t;

typedef struct {
    pomo_day_rec_t days[POMO_STATS_DAYS];  /* 环形，head 指向最新 */
    uint16_t head;
    uint16_t count;
    uint32_t total_pomos;
    uint32_t total_focus_min;
} pomo_stats_t;

void pomo_stats_init(pomo_stats_t *st);

/* date 为 POMO_NO_DATE 时只累计总量。时钟回拨的旧日期在环内命中则补记，
 * 否则只累计(不破坏环形日期有序性)。 */
void pomo_stats_record(pomo_stats_t *st, uint16_t date, uint16_t minutes);

bool pomo_stats_day(const pomo_stats_t *st, uint16_t date,
                    uint16_t *pomos, uint16_t *minutes);

/* out[0] = today-6 .. out[6] = today，无记录的天数置 0。 */
void pomo_stats_last7(const pomo_stats_t *st, uint16_t today,
                      pomo_day_rec_t out[7]);

/* today 所在周(周一起)至 today 的合计。 */
void pomo_stats_week(const pomo_stats_t *st, uint16_t today,
                     uint32_t *pomos, uint32_t *minutes);
