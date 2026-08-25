// tests/test_battery_gauge.c —— SOC readiness/plausibility 纯逻辑测试
#include <assert.h>
#include <stdio.h>

#include "battery_gauge.h"

static uint16_t raw_of(int pct) { return (uint16_t)(pct << 8); }

int main(void) {
    /* ---- bg_sample_plausible ---- */

    // 用户场景：raw=0x0000 + 高电压 → NOT READY（startup 默认值）
    assert(!bg_sample_plausible(true, 0x0000, 4100));
    assert(!bg_sample_plausible(true, 0x0000, 4080));
    assert(!bg_sample_plausible(true, 0x0000, 3600));   // 边界：>=3600 即矛盾

    // 真空电（低电压下 raw=0 是物理可能的）
    assert(bg_sample_plausible(true, 0x0000, 3500));
    assert(bg_sample_plausible(true, 0x0000, 3599));

    // 50% + 3800mV → 合法候选
    assert(bg_sample_plausible(true, raw_of(50), 3800));

    // >100（0xFF 等未就绪值）→ 不可信
    assert(!bg_sample_plausible(true, 0xFF00, 3900));
    assert(!bg_sample_plausible(true, raw_of(101), 3900));

    // I2C 失败 → 不可信
    assert(!bg_sample_plausible(false, 0x4380, 3900));

    /* ---- WAIT_READY：连续样本门槛 ---- */

    bg_ctx_t c;
    bg_init(&c);
    assert(c.state == BG_WAIT_READY);
    assert(bg_display_soc(&c) == -1);

    // 单次 valid → 不 READY
    bg_feed(&c, true, raw_of(50), 3800);
    assert(c.state == BG_WAIT_READY);

    // 连续 3 次 valid → READY
    bg_init(&c);
    assert(!bg_feed(&c, true, raw_of(68), 3920));
    assert(!bg_feed(&c, true, raw_of(68), 3920));
    assert(bg_feed(&c, true, raw_of(68), 3920));
    assert(c.state == BG_READY);
    assert(bg_display_soc(&c) == 68);

    // 0% 矛盾样本永不累积 streak：喂 10 次 0x0000/4100mV 仍 WAIT_READY
    bg_init(&c);
    for (int i = 0; i < 10; i++) bg_feed(&c, true, 0x0000, 4100);
    assert(c.state == BG_WAIT_READY);
    assert(bg_display_soc(&c) == -1);

    // streak 被一次不可信样本打断后重新计数
    bg_init(&c);
    bg_feed(&c, true, raw_of(50), 3800);       // streak 1
    bg_feed(&c, true, 0x0000, 4100);           // 打断 → 0
    bg_feed(&c, true, raw_of(50), 3800);       // 1
    bg_feed(&c, true, raw_of(50), 3800);       // 2
    assert(c.state == BG_WAIT_READY);
    bg_feed(&c, true, raw_of(50), 3800);       // 3 → READY
    assert(c.state == BG_READY);

    // 观察窗口超时 → force_fallback
    bg_init(&c);
    bg_feed(&c, true, 0x0000, 4100);
    bg_force_fallback(&c);
    assert(c.state == BG_FALLBACK);
    assert(bg_display_soc(&c) == -1);
    // force_fallback 不影响已 READY 的状态
    bg_init(&c);
    for (int i = 0; i < 3; i++) bg_feed(&c, true, raw_of(80), 4000);
    bg_force_fallback(&c);
    assert(c.state == BG_READY);

    /* ---- READY 态：容错与退化 ---- */

    bg_init(&c);
    for (int i = 0; i < 3; i++) bg_feed(&c, true, raw_of(68), 3920);
    assert(c.state == BG_READY);

    // 单次异常 → 不退出 READY，显示保持
    assert(!bg_feed(&c, true, 0x0000, 4100));
    assert(c.state == BG_READY);
    assert(bg_display_soc(&c) == 68);

    // 连续 3 次异常 → FALLBACK
    assert(!bg_feed(&c, true, 0x0000, 4100));   // 第 2 次
    assert(bg_feed(&c, true, 0x0000, 4100));    // 第 3 次
    assert(c.state == BG_FALLBACK);
    assert(bg_display_soc(&c) == -1);

    // READY 态 SOC 正常缓慢变化直接跟随
    bg_init(&c);
    for (int i = 0; i < 3; i++) bg_feed(&c, true, raw_of(67), 3920);
    assert(bg_feed(&c, true, raw_of(66), 3910));
    assert(bg_display_soc(&c) == 66);

    // I2C 连续失败 3 次（如拔线）→ FALLBACK；短暂失败被容忍
    bg_init(&c);
    for (int i = 0; i < 3; i++) bg_feed(&c, true, raw_of(80), 4000);
    bg_feed(&c, false, 0, 0);
    bg_feed(&c, false, 0, 0);
    assert(c.state == BG_READY);
    assert(bg_feed(&c, false, 0, 0));
    assert(c.state == BG_FALLBACK);

    /* ---- FALLBACK：回到 READY 同样需要连续样本 ---- */

    // 单次 valid → 不进入 READY
    assert(!bg_feed(&c, true, raw_of(67), 3920));
    assert(c.state == BG_FALLBACK);

    // 连续 3 次 valid → READY
    assert(!bg_feed(&c, true, raw_of(67), 3920));
    assert(bg_feed(&c, true, raw_of(67), 3920));
    assert(c.state == BG_READY);
    assert(bg_display_soc(&c) == 67);

    // FALLBACK 态 0% 矛盾样本不累积 streak
    bg_init(&c);
    bg_force_fallback(&c);
    for (int i = 0; i < 10; i++) bg_feed(&c, true, 0x0000, 4120);
    assert(c.state == BG_FALLBACK);

    printf("battery_gauge: all tests passed\n");
    return 0;
}
