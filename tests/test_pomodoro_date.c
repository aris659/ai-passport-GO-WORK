#include "pomodoro_date.h"

#include <assert.h>
#include <stdio.h>

/* 与实现内部一致的民用日期换算，供往返验证 */
static int64_t days_from_civil_local(int year, int month, int day) {
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int yoe = year - (int)era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void test_known_dates(void) {
    /* 2026-08-24 是周一，epoch 天数 20689 */
    int y, m, d;
    pomo_date_to_ymd(20689, &y, &m, &d);
    assert(y == 2026 && m == 8 && d == 24);
    assert(pomo_weekday(20689) == 0);
    assert(pomo_week_start(20689) == 20689);
    assert(pomo_week_start(20690) == 20689);
    assert(pomo_week_start(20695) == 20689);   /* 周日 */
    assert(pomo_week_start(20696) == 20696);   /* 下周一 */
    assert(pomo_weekday(20695) == 6);
}

static void test_epoch_anchor(void) {
    int y, m, d;
    pomo_date_to_ymd(0, &y, &m, &d);
    assert(y == 1970 && m == 1 && d == 1);
    assert(pomo_weekday(0) == 3);              /* 周四 */
    /* epoch 前的周一为 -3 天，uint16_t 下模 65536 回绕 */
    assert(pomo_week_start(0) == (uint16_t)(0 - 3));
}

static void test_tz_boundary(void) {
    /* 本地日 D 从 unix D*86400-28800 开始 */
    uint16_t day = 20689;
    int64_t midnight = pomo_unix_at_local_midnight(day);
    assert(midnight == (int64_t)day * 86400 - 8 * 3600);
    assert(pomo_date_from_unix(midnight) == day);
    assert(pomo_date_from_unix(midnight + 23 * 3600 + 59 * 60 + 59) == day);
    assert(pomo_date_from_unix(midnight - 1) == day - 1);
    /* UTC 23:30 属本地次日 */
    assert(pomo_date_from_unix((int64_t)day * 86400 + 23 * 3600 + 1800) == day + 1);
}

static void test_ymd_roundtrip(void) {
    for (int64_t days = 0; days <= 30000; days += 97) {
        int y, m, d;
        pomo_date_to_ymd((uint16_t)days, &y, &m, &d);
        assert(days_from_civil_local(y, m, d) == days);
    }
}

static void test_estimate(void) {
    /* 无锚点不可用 */
    assert(pomo_time_estimate_unix(0, 5000, 9000) == 0);
    assert(pomo_time_estimate_unix(-1, 5000, 9000) == 0);

    /* 正常推算：流逝 4 秒 */
    assert(pomo_time_estimate_unix(1770000000LL, 5000, 9000) == 1770000004LL);
    /* 毫秒截断：1500ms 只进 1 秒 */
    assert(pomo_time_estimate_unix(1000LL, 0, 1500) == 1001LL);
    /* now 早于锚点时刻：钳回锚点 */
    assert(pomo_time_estimate_unix(1770000000LL, 9000, 5000) == 1770000000LL);
    assert(pomo_time_estimate_unix(1770000000LL, 9000, 9000) == 1770000000LL);
    /* 长时间运行不溢出：约 10 年 */
    assert(pomo_time_estimate_unix(1770000000LL, 0, 315360000000ULL) ==
           1770000000LL + 315360000LL);
}

int main(void) {
    test_known_dates();
    test_epoch_anchor();
    test_tz_boundary();
    test_ymd_roundtrip();
    test_estimate();
    puts("pomodoro_date: all tests passed");
    return 0;
}
