#include "pomodoro_stats.h"

#include <string.h>

#include "pomodoro_date.h"

static pomo_day_rec_t *rec_at(pomo_stats_t *st, uint16_t back) {
    uint16_t idx = (uint16_t)((st->head + POMO_STATS_DAYS - back) % POMO_STATS_DAYS);
    return &st->days[idx];
}

static const pomo_day_rec_t *rec_at_const(const pomo_stats_t *st, uint16_t back) {
    uint16_t idx = (uint16_t)((st->head + POMO_STATS_DAYS - back) % POMO_STATS_DAYS);
    return &st->days[idx];
}

static void add_to_rec(pomo_day_rec_t *rec, uint16_t minutes) {
    if (rec->pomos < 0xFFFF) rec->pomos++;
    if (rec->focus_min <= 0xFFFF - minutes) rec->focus_min += minutes;
}

void pomo_stats_init(pomo_stats_t *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
}

void pomo_stats_record(pomo_stats_t *st, uint16_t date, uint16_t minutes) {
    if (!st) return;
    st->total_pomos++;
    st->total_focus_min += minutes;
    if (date == POMO_NO_DATE) return;

    if (st->count > 0) {
        const pomo_day_rec_t *newest = rec_at_const(st, 0);
        if (newest->date == date) {
            add_to_rec(rec_at(st, 0), minutes);
            return;
        }
        if (date < newest->date) {
            for (uint16_t back = 1; back < st->count; back++) {
                pomo_day_rec_t *rec = rec_at(st, back);
                if (rec->date == date) {
                    add_to_rec(rec, minutes);
                    return;
                }
            }
            return;  /* 早于环内所有记录：只计累计 */
        }
    }

    st->head = (uint16_t)((st->head + 1) % POMO_STATS_DAYS);
    st->days[st->head].date = date;
    st->days[st->head].pomos = 1;
    st->days[st->head].focus_min = minutes;
    if (st->count < POMO_STATS_DAYS) st->count++;
}

bool pomo_stats_day(const pomo_stats_t *st, uint16_t date,
                    uint16_t *pomos, uint16_t *minutes) {
    if (pomos) *pomos = 0;
    if (minutes) *minutes = 0;
    if (!st) return false;
    for (uint16_t back = 0; back < st->count; back++) {
        const pomo_day_rec_t *rec = rec_at_const(st, back);
        if (rec->date == date) {
            if (pomos) *pomos = rec->pomos;
            if (minutes) *minutes = rec->focus_min;
            return true;
        }
        if (rec->date < date) break;  /* 环内日期有序，可提前终止 */
    }
    return false;
}

void pomo_stats_last7(const pomo_stats_t *st, uint16_t today,
                      pomo_day_rec_t out[7]) {
    for (int i = 0; i < 7; i++) {
        int offset = 6 - i;
        int d = (int)today - offset;
        out[i].date = d < 0 ? 0 : (uint16_t)d;
        out[i].pomos = 0;
        out[i].focus_min = 0;
        pomo_stats_day(st, out[i].date, &out[i].pomos, &out[i].focus_min);
    }
}

void pomo_stats_week(const pomo_stats_t *st, uint16_t today,
                     uint32_t *pomos, uint32_t *minutes) {
    if (pomos) *pomos = 0;
    if (minutes) *minutes = 0;
    if (!st) return;
    uint16_t start = pomo_week_start(today);
    for (int i = 0; i < 7; i++) {
        uint16_t d = (uint16_t)(start + i);
        if (d > today) break;
        uint16_t p = 0, m = 0;
        pomo_stats_day(st, d, &p, &m);
        if (pomos) *pomos += p;
        if (minutes) *minutes += m;
    }
}
