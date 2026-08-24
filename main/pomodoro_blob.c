#include "pomodoro_blob.h"

#include <string.h>

/* v1 档位表 {15, 25, 45}，迁移时按分钟映射到新档位。 */
static const uint8_t V1_FOCUS_MINUTES[3] = {15, 25, 45};

uint32_t pomo_blob_crc32(const void *data, size_t len) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1));
        }
    }
    return ~crc;
}

void pomo_blob_encode(pomo_blob_v2_t *out,
                      const pomodoro_model_t *model,
                      const pomo_stats_t *stats,
                      int64_t anchor_unix) {
    if (!out || !model || !stats) return;
    memset(out, 0, sizeof(*out));
    out->magic = POMO_BLOB_MAGIC;
    out->version = POMO_BLOB_VERSION;
    out->size = sizeof(pomo_blob_v2_t);
    out->state = (uint8_t)model->state;
    out->selected_index = model->selected_index;
    out->reward_pending = model->reward_pending;
    out->pomodoro_round = model->pomodoro_round;
    out->pending_break_min = model->pending_break_min;
    out->muted = model->muted;
    out->remaining_sec = model->remaining_sec;
    out->session_id = model->session_id;
    out->break_remaining_sec = model->break_remaining_sec;
    out->anchor_unix = anchor_unix;
    out->stats = *stats;
    out->crc32 = pomo_blob_crc32(out, offsetof(pomo_blob_v2_t, crc32));
}

static bool blob_header_ok(const void *data, size_t len) {
    const pomo_blob_v2_t *blob = data;  /* v1/v2 头部前 8 字节布局一致 */
    return data && len >= 8 && blob->magic == POMO_BLOB_MAGIC;
}

static void decode_v1(const pomo_blob_v1_t *blob, pomodoro_model_t *model,
                      pomo_stats_t *stats, int64_t *anchor_unix) {
    pomodoro_model_defaults(model);
    pomo_stats_init(stats);
    *anchor_unix = 0;

    model->state = (pomodoro_state_t)blob->state;
    if (blob->selected_index < 3) {
        model->selected_index =
            pomodoro_model_index_of(V1_FOCUS_MINUTES[blob->selected_index]);
    }
    model->reward_pending = blob->reward_pending != 0;
    model->pomodoro_round = blob->pomodoro_round;
    model->pending_break_min = blob->pending_break_min;
    model->muted = blob->muted != 0;
    model->remaining_sec = blob->remaining_sec;
    model->session_id = blob->session_id;
    model->break_remaining_sec = blob->break_remaining_sec;

    /* 逐日桶无处迁移，仅继承两项累计。 */
    stats->total_pomos = blob->completed_sessions;
    stats->total_focus_min = blob->completed_focus_min;

    pomodoro_model_restore(model);
}

static void decode_v2(const pomo_blob_v2_t *blob, pomodoro_model_t *model,
                      pomo_stats_t *stats, int64_t *anchor_unix) {
    pomodoro_model_defaults(model);
    pomo_stats_init(stats);
    *anchor_unix = blob->anchor_unix;

    model->state = (pomodoro_state_t)blob->state;
    model->selected_index = blob->selected_index;
    model->reward_pending = blob->reward_pending != 0;
    model->pomodoro_round = blob->pomodoro_round;
    model->pending_break_min = blob->pending_break_min;
    model->muted = blob->muted != 0;
    model->remaining_sec = blob->remaining_sec;
    model->session_id = blob->session_id;
    model->break_remaining_sec = blob->break_remaining_sec;

    *stats = blob->stats;
    if (stats->head >= POMO_STATS_DAYS || stats->count > POMO_STATS_DAYS) {
        pomo_stats_init(stats);
        stats->total_pomos = blob->stats.total_pomos;
        stats->total_focus_min = blob->stats.total_focus_min;
    }

    pomodoro_model_restore(model);
}

pomo_blob_decode_result_t pomo_blob_decode(const void *data, size_t len,
                                           pomodoro_model_t *model,
                                           pomo_stats_t *stats,
                                           int64_t *anchor_unix) {
    if (!model || !stats || !anchor_unix) return POMO_BLOB_DECODE_NONE;
    pomodoro_model_defaults(model);
    pomo_stats_init(stats);
    *anchor_unix = 0;
    if (!blob_header_ok(data, len)) return POMO_BLOB_DECODE_NONE;

    const pomo_blob_v2_t *blob = data;  /* 头部前 8 字节布局一致 */
    if (blob->version == 1 && len == sizeof(pomo_blob_v1_t)) {
        const pomo_blob_v1_t *v1 = data;
        if (v1->size == sizeof(pomo_blob_v1_t) &&
            v1->crc32 == pomo_blob_crc32(v1, offsetof(pomo_blob_v1_t, crc32))) {
            decode_v1(v1, model, stats, anchor_unix);
            return POMO_BLOB_DECODE_V1;
        }
        return POMO_BLOB_DECODE_NONE;
    }

    if (blob->version == POMO_BLOB_VERSION && len == sizeof(pomo_blob_v2_t)) {
        if (blob->size == sizeof(pomo_blob_v2_t) &&
            blob->crc32 == pomo_blob_crc32(blob, offsetof(pomo_blob_v2_t, crc32))) {
            decode_v2(blob, model, stats, anchor_unix);
            return POMO_BLOB_DECODE_V2;
        }
    }
    return POMO_BLOB_DECODE_NONE;
}
