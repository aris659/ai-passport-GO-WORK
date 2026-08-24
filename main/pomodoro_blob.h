#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pomodoro_model.h"
#include "pomodoro_stats.h"

#define POMO_BLOB_MAGIC 0x504F4D4DU
#define POMO_BLOB_VERSION 2

/* v1 线上布局，仅用于读取迁移，字段顺序不可改动。 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t state;
    uint8_t selected_index;
    uint8_t reward_pending;
    uint8_t pomodoro_round;
    uint8_t pending_break_min;
    uint8_t muted;
    uint16_t reserved;
    uint32_t remaining_sec;
    uint32_t session_id;
    uint32_t completed_sessions;
    uint32_t completed_focus_min;
    uint32_t break_remaining_sec;
    uint32_t crc32;
} pomo_blob_v1_t;

/* v2 布局：状态 + 时间锚点 + 统计。 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint8_t state;
    uint8_t selected_index;
    uint8_t reward_pending;
    uint8_t pomodoro_round;
    uint8_t pending_break_min;
    uint8_t muted;
    uint16_t reserved;
    uint32_t remaining_sec;
    uint32_t session_id;
    uint32_t break_remaining_sec;
    uint32_t reserved2;
    int64_t anchor_unix;
    pomo_stats_t stats;
    uint32_t crc32;
} pomo_blob_v2_t;

typedef enum {
    POMO_BLOB_DECODE_NONE = 0,  /* 无效或缺失，输出已重置为默认值 */
    POMO_BLOB_DECODE_V1,        /* 读到 v1 并完成迁移 */
    POMO_BLOB_DECODE_V2,        /* 读到 v2 */
} pomo_blob_decode_result_t;

pomo_blob_decode_result_t pomo_blob_decode(const void *data, size_t len,
                                           pomodoro_model_t *model,
                                           pomo_stats_t *stats,
                                           int64_t *anchor_unix);

void pomo_blob_encode(pomo_blob_v2_t *out,
                      const pomodoro_model_t *model,
                      const pomo_stats_t *stats,
                      int64_t anchor_unix);

uint32_t pomo_blob_crc32(const void *data, size_t len);
