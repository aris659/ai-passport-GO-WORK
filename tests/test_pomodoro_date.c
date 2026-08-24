#include "pomodoro_date.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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

static void test_weekday_letters(void) {
    char out[8] = {0};
    /* 20689 = 2026-08-24 周一：近 7 天为上周二..今天 */
    pomo_weekday_letters(20689, out);
    assert(strcmp(out, "TWTFSSM") == 0);
    /* 20691 = 周三：上周四..本周三 */
    pomo_weekday_letters(20691, out);
    assert(strcmp(out, "TFSSMTW") == 0);
    /* 20695 = 周日：本周一..本周日（唯一与固定 MTWTFSS 重合的情形） */
    pomo_weekday_letters(20695, out);
    assert(strcmp(out, "MTWTFSS") == 0);
    /* 连续 7 天滑动：序列每天左移一格 */
    char prev[8] = {0};
    pomo_weekday_letters(20689, prev);
    for (uint16_t d = 20690; d <= 20695; d++) {
        pomo_weekday_letters(d, out);
        assert(out[6] == "MTWTFSS"[(d + 3) % 7]);
        assert(memcmp(out, prev + 1, 6) == 0);
        memcpy(prev, out, 7);
    }
}

/* 与 demo_pomodoro.c pix_text_width 同式：advance=6*scale 去尾 gap */
static int pix_w(const char *s, int scale) {
    int n = (int)strlen(s);
    return n > 0 ? n * 6 * scale - scale : 0;
}

static void test_summary_fit(void) {
    char buf[40];
    /* 短数字：完整文案 scale2（18 字符 = 214px <= 230） */
    assert(pomo_summary_fit(buf, sizeof(buf), true, 0, 0, 230) == 2);
    assert(strcmp(buf, "TODAY 0 WHIPS 0 MIN") == 0);
    /* 常规数字：完整文案 238px 超限 -> 紧凑文案 scale2 */
    assert(pomo_summary_fit(buf, sizeof(buf), true, 3, 75, 230) == 2);
    assert(strcmp(buf, "3 WHIPS 75 MIN") == 0);
    /* 用户报告的溢出案例 */
    assert(pomo_summary_fit(buf, sizeof(buf), true, 10, 250, 230) == 2);
    assert(strcmp(buf, "10 WHIPS 250 MIN") == 0);
    /* 无时间基准：-- 占位同样走紧凑退化 */
    assert(pomo_summary_fit(buf, sizeof(buf), false, 0, 0, 230) == 2);
    assert(strcmp(buf, "-- WHIPS -- MIN") == 0);
    /* uint16 极值：紧凑文案 250px 仍超限 -> 降 scale1 */
    assert(pomo_summary_fit(buf, sizeof(buf), true, 65535, 65535, 230) == 1);
    assert(strcmp(buf, "65535 WHIPS 65535 MIN") == 0);
    /* 性质测试：任意组合下渲染宽度都在安全区内 */
    static const unsigned V[] = {0, 1, 9, 10, 99, 100, 999, 1000, 9999, 65535};
    for (size_t i = 0; i < sizeof(V) / sizeof(V[0]); i++) {
        for (size_t j = 0; j < sizeof(V) / sizeof(V[0]); j++) {
            int scale = pomo_summary_fit(buf, sizeof(buf), true, V[i], V[j], 230);
            assert(scale == 1 || scale == 2);
            assert(pix_w(buf, scale) <= 230);
            /* 宽度必须居中可用：不超过屏宽 */
            assert(pix_w(buf, scale) <= 240);
        }
    }
}

int main(void) {
    test_known_dates();
    test_epoch_anchor();
    test_tz_boundary();
    test_ymd_roundtrip();
    test_estimate();
    test_weekday_letters();
    test_summary_fit();
    puts("pomodoro_date: all tests passed");
    return 0;
}
