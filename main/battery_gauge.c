// main/battery_gauge.c —— 见 battery_gauge.h
#include "battery_gauge.h"

void bg_init(bg_ctx_t *ctx) {
    ctx->state = BG_WAIT_READY;
    ctx->valid_streak = 0;
    ctx->invalid_streak = 0;
    ctx->soc = -1;
}

bool bg_sample_plausible(bool read_ok, uint16_t soc_raw, int voltage_mv) {
    if (!read_ok) return false;
    int soc_int = (soc_raw >> 8) & 0xFF;
    if (soc_int > 100) return false;                  // 0xFF 等未就绪值
    if (soc_raw == 0x0000 && voltage_mv >= BG_ZERO_CONFLICT_MV)
        return false;                                  // 上电默认 0 与满电矛盾
    return true;
}

bool bg_feed(bg_ctx_t *ctx, bool read_ok, uint16_t soc_raw, int voltage_mv) {
    bool plausible = bg_sample_plausible(read_ok, soc_raw, voltage_mv);
    int soc_int = (int)((soc_raw >> 8) & 0xFF);
    bg_state_t prev_state = ctx->state;
    int prev_soc = ctx->soc;

    switch (ctx->state) {
    case BG_WAIT_READY:
    case BG_FALLBACK:
        if (plausible) {
            if (ctx->valid_streak < BG_READY_STREAK) ctx->valid_streak++;
            if (ctx->valid_streak >= BG_READY_STREAK) {
                ctx->state = BG_READY;
                ctx->soc = (int16_t)soc_int;
                ctx->valid_streak = 0;
                ctx->invalid_streak = 0;
            }
        } else {
            ctx->valid_streak = 0;
        }
        break;

    case BG_READY:
        if (plausible) {
            ctx->invalid_streak = 0;
            ctx->soc = (int16_t)soc_int;   // 正常缓慢变化直接跟随
        } else {
            // 容忍短暂单次错误；连续异常才退回 FALLBACK
            if (ctx->invalid_streak < BG_LOST_STREAK) ctx->invalid_streak++;
            if (ctx->invalid_streak >= BG_LOST_STREAK) {
                ctx->state = BG_FALLBACK;
                ctx->soc = -1;
                ctx->valid_streak = 0;
                ctx->invalid_streak = 0;
            }
        }
        break;
    }
    return ctx->state != prev_state || ctx->soc != prev_soc;
}

void bg_force_fallback(bg_ctx_t *ctx) {
    if (ctx->state == BG_READY) return;
    ctx->state = BG_FALLBACK;
    ctx->valid_streak = 0;
    ctx->invalid_streak = 0;
}

int bg_display_soc(const bg_ctx_t *ctx) {
    return ctx->state == BG_READY ? ctx->soc : -1;
}
