#include "pomodoro_stats.h"

#include <assert.h>
#include <stdio.h>

static void test_record_and_query(void) {
    pomo_stats_t st;
    pomo_stats_init(&st);
    assert(st.count == 0 && st.total_pomos == 0 && st.total_focus_min == 0);

    pomo_stats_record(&st, 100, 25);
    pomo_stats_record(&st, 100, 45);
    uint16_t p = 0, m = 0;
    assert(pomo_stats_day(&st, 100, &p, &m));
    assert(p == 2 && m == 70);
    assert(st.total_pomos == 2 && st.total_focus_min == 70);
    assert(!pomo_stats_day(&st, 101, &p, &m));
    assert(p == 0 && m == 0);

    pomo_stats_record(&st, 101, 5);
    assert(pomo_stats_day(&st, 100, &p, &m) && p == 2 && m == 70);
    assert(pomo_stats_day(&st, 101, &p, &m) && p == 1 && m == 5);
}

static void test_no_date_only_totals(void) {
    pomo_stats_t st;
    pomo_stats_init(&st);
    pomo_stats_record(&st, POMO_NO_DATE, 25);
    pomo_stats_record(&st, POMO_NO_DATE, 90);
    assert(st.total_pomos == 2 && st.total_focus_min == 115);
    uint16_t p, m;
    assert(!pomo_stats_day(&st, POMO_NO_DATE, &p, &m));
    assert(st.count == 0);
}

static void test_clock_rollback(void) {
    pomo_stats_t st;
    pomo_stats_init(&st);
    pomo_stats_record(&st, 100, 25);
    pomo_stats_record(&st, 101, 25);
    pomo_stats_record(&st, 105, 25);

    /* 回拨到环内已有日期：补记到原桶 */
    pomo_stats_record(&st, 101, 10);
    uint16_t p, m;
    assert(pomo_stats_day(&st, 101, &p, &m) && p == 2 && m == 35);

    /* 回拨到环内不存在且早于最旧(100)的日期：只计累计 */
    uint32_t before = st.total_pomos;
    pomo_stats_record(&st, 90, 25);
    assert(st.total_pomos == before + 1);
    assert(!pomo_stats_day(&st, 90, &p, &m));

    /* 之后的新日期不受影响 */
    pomo_stats_record(&st, 106, 25);
    assert(pomo_stats_day(&st, 106, &p, &m) && p == 1 && m == 25);
}

static void test_ring_eviction(void) {
    pomo_stats_t st;
    pomo_stats_init(&st);
    for (uint16_t day = 1000; day < 1000 + POMO_STATS_DAYS; day++) {
        pomo_stats_record(&st, day, 5);
    }
    assert(st.count == POMO_STATS_DAYS);
    uint16_t p, m;
    assert(pomo_stats_day(&st, 1000, &p, &m) && p == 1 && m == 5);

    pomo_stats_record(&st, 1000 + POMO_STATS_DAYS, 5);
    assert(st.count == POMO_STATS_DAYS);
    assert(!pomo_stats_day(&st, 1000, &p, &m));       /* 最旧被淘汰 */
    assert(pomo_stats_day(&st, 1001, &p, &m) && p == 1);
    assert(st.total_pomos == POMO_STATS_DAYS + 1);
}

static void test_last7_and_week(void) {
    pomo_stats_t st;
    pomo_stats_init(&st);
    /* 让 20689(周一) 为"今天"，此前几天各记一些 */
    pomo_stats_record(&st, 20683, 25);  /* 上周二 */
    pomo_stats_record(&st, 20687, 25);  /* 周六 */
    pomo_stats_record(&st, 20688, 45);  /* 周日 */
    pomo_stats_record(&st, 20689, 25);  /* 周一(今天) */

    pomo_day_rec_t week[7];
    pomo_stats_last7(&st, 20689, week);
    assert(week[0].date == 20683 && week[0].focus_min == 25);
    assert(week[1].pomos == 0 && week[1].focus_min == 0);
    assert(week[6].date == 20689 && week[6].pomos == 1 && week[6].focus_min == 25);

    uint32_t wp, wm;
    pomo_stats_week(&st, 20689, &wp, &wm);
    assert(wp == 1 && wm == 25);       /* 本周(周一起)只有周一 */

    /* 周三视角：本周含周一与周三，周六周日属上周不计 */
    pomo_stats_record(&st, 20691, 15);  /* 周三 */
    pomo_stats_week(&st, 20691, &wp, &wm);
    assert(wp == 2 && wm == 40);
}

int main(void) {
    test_record_and_query();
    test_no_date_only_totals();
    test_clock_rollback();
    test_ring_eviction();
    test_last7_and_week();
    puts("pomodoro_stats: all tests passed");
    return 0;
}
