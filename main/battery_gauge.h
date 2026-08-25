// main/battery_gauge.h
// CW2017 SOC readiness 纯逻辑判定：不碰寄存器、不依赖 ESP-IDF，host 可测。
// 核心原则：SOC raw 落在 0..100 不等于可信——
// 芯片上电默认 SOC=0x0000（CW2017-DS V1.1），与 VCELL>=3.6V 明显矛盾，
// 必须视为 NOT READY，绝不能显示 0%。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BG_READY_STREAK      3     // 进入 READY 需要的连续可信样本数
#define BG_LOST_STREAK       3     // READY 退回 FALLBACK 需要的连续异常样本数
#define BG_ZERO_CONFLICT_MV  3600  // SOC==0 与该电压以上矛盾（LiPo 物理不可能）

typedef enum {
    BG_WAIT_READY = 0,   // 快速观察窗口（超时由调用方控制）
    BG_READY,            // SOC 可信，可显示百分比
    BG_FALLBACK,         // SOC 不可信；上层显示实测电压
} bg_state_t;

typedef struct {
    bg_state_t state;
    uint8_t valid_streak;    // 当前连续可信样本数
    uint8_t invalid_streak;  // READY 态连续异常样本数
    int16_t soc;             // 可信 SOC 0..100；-1 = 无
} bg_ctx_t;

void bg_init(bg_ctx_t *ctx);

// 单样本可信性：读取成功 + 范围合法 + 与电压不矛盾。
// 这里不做电压→SOC 换算，只用电压识别明显不可能的状态。
bool bg_sample_plausible(bool read_ok, uint16_t soc_raw, int voltage_mv);

// 喂入一个样本，推进状态机。
// 返回 true = 状态或显示 SOC 变化（调用方应刷 UI）。
bool bg_feed(bg_ctx_t *ctx, bool read_ok, uint16_t soc_raw, int voltage_mv);

// 观察窗口超时：仍未获得可信 SOC 时强制进入 FALLBACK（不判芯片坏）。
void bg_force_fallback(bg_ctx_t *ctx);

// 显示 SOC；-1 = 无（上层显示电压或 --%）。
int bg_display_soc(const bg_ctx_t *ctx);
