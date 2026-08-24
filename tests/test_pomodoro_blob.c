#include "pomodoro_blob.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static pomo_blob_v1_t make_v1(uint8_t state, uint8_t selected_index,
                              uint32_t sessions, uint32_t focus_min) {
    pomo_blob_v1_t b;
    memset(&b, 0, sizeof(b));
    b.magic = POMO_BLOB_MAGIC;
    b.version = 1;
    b.size = sizeof(pomo_blob_v1_t);
    b.state = state;
    b.selected_index = selected_index;
    b.reward_pending = 1;
    b.pomodoro_round = 2;
    b.pending_break_min = 5;
    b.muted = 1;
    b.remaining_sec = 1234;
    b.session_id = 42;
    b.completed_sessions = sessions;
    b.completed_focus_min = focus_min;
    b.break_remaining_sec = 300;
    b.crc32 = pomo_blob_crc32(&b, offsetof(pomo_blob_v1_t, crc32));
    return b;
}

static void fill_model(pomodoro_model_t *model) {
    pomodoro_model_defaults(model);
    model->state = POMODORO_FOCUS_PAUSED;
    model->selected_index = 6;  /* 45 分钟 */
    model->reward_pending = false;
    model->pomodoro_round = 3;
    model->pending_break_min = 10;
    model->muted = true;
    model->remaining_sec = 777;
    model->session_id = 9;
    model->break_remaining_sec = 44;
}

static void fill_stats(pomo_stats_t *stats) {
    pomo_stats_init(stats);
    pomo_stats_record(stats, 20688, 45);
    pomo_stats_record(stats, 20689, 25);
    pomo_stats_record(stats, 20689, 30);
}

static void test_v2_roundtrip(void) {
    pomodoro_model_t model;
    pomo_stats_t stats;
    fill_model(&model);
    fill_stats(&stats);

    pomo_blob_v2_t blob;
    pomo_blob_encode(&blob, &model, &stats, 1770000000LL);

    pomodoro_model_t out_model;
    pomo_stats_t out_stats;
    int64_t anchor = 0;
    assert(pomo_blob_decode(&blob, sizeof(blob), &out_model, &out_stats,
                            &anchor) == POMO_BLOB_DECODE_V2);
    assert(anchor == 1770000000LL);
    assert(out_model.state == POMODORO_FOCUS_PAUSED);
    assert(out_model.selected_index == 6);
    assert(!out_model.reward_pending);
    assert(out_model.pomodoro_round == 3);
    assert(out_model.pending_break_min == 10);
    assert(out_model.muted);
    assert(out_model.remaining_sec == 777);
    assert(out_model.session_id == 9);
    assert(out_model.break_remaining_sec == 44);

    assert(out_stats.count == 2);
    assert(out_stats.total_pomos == 3);
    assert(out_stats.total_focus_min == 100);
    uint16_t p = 0, m = 0;
    assert(pomo_stats_day(&out_stats, 20689, &p, &m) && p == 2 && m == 55);
    assert(pomo_stats_day(&out_stats, 20688, &p, &m) && p == 1 && m == 45);
}

static void test_v1_migration(void) {
    pomo_blob_v1_t v1 = make_v1(POMODORO_FOCUS_PAUSED, 1, 7, 100);
    v1.reward_pending = 0;
    v1.crc32 = pomo_blob_crc32(&v1, offsetof(pomo_blob_v1_t, crc32));

    pomodoro_model_t model;
    pomo_stats_t stats;
    int64_t anchor = 0;
    assert(pomo_blob_decode(&v1, sizeof(v1), &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_V1);

    /* 档位按分钟映射：v1 idx1(25min) -> v2 idx4(25min) */
    assert(model.selected_index == 4);
    assert(pomodoro_model_focus_min(&model) == 25);
    /* 运行中状态恢复为暂停 */
    assert(model.state == POMODORO_FOCUS_PAUSED);
    assert(model.remaining_sec == 1234);
    assert(model.session_id == 42);
    assert(model.pomodoro_round == 2);
    assert(model.pending_break_min == 5);
    assert(model.muted);
    assert(!model.reward_pending);

    /* 累计继承，逐日桶为空，锚点归零 */
    assert(stats.total_pomos == 7);
    assert(stats.total_focus_min == 100);
    assert(stats.count == 0);
    assert(anchor == 0);

    /* v1 的 reward_pending 使状态归为 REWARD */
    v1.reward_pending = 1;
    v1.crc32 = pomo_blob_crc32(&v1, offsetof(pomo_blob_v1_t, crc32));
    assert(pomo_blob_decode(&v1, sizeof(v1), &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_V1);
    assert(model.reward_pending);
    assert(model.state == POMODORO_REWARD);
}

static void test_v1_index_mapping_all(void) {
    static const uint8_t EXPECT[] = {2, 4, 6};  /* 15/25/45 -> 新表下标 */
    for (uint8_t i = 0; i < 3; i++) {
        pomo_blob_v1_t v1 = make_v1(POMODORO_IDLE, i, 0, 0);
        pomodoro_model_t model;
        pomo_stats_t stats;
        int64_t anchor = 0;
        assert(pomo_blob_decode(&v1, sizeof(v1), &model, &stats, &anchor) ==
               POMO_BLOB_DECODE_V1);
        assert(model.selected_index == EXPECT[i]);
    }

    /* v1 非法下标回退到新默认档 */
    pomo_blob_v1_t bad = make_v1(POMODORO_IDLE, 9, 0, 0);
    pomodoro_model_t model;
    pomo_stats_t stats;
    int64_t anchor = 0;
    assert(pomo_blob_decode(&bad, sizeof(bad), &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_V1);
    assert(model.selected_index == POMODORO_DEFAULT_INDEX);
}

static void assert_defaults(const pomodoro_model_t *model,
                            const pomo_stats_t *stats, int64_t anchor) {
    assert(model->state == POMODORO_IDLE);
    assert(model->selected_index == POMODORO_DEFAULT_INDEX);
    assert(!model->muted && !model->reward_pending);
    assert(stats->count == 0 && stats->total_pomos == 0 &&
           stats->total_focus_min == 0);
    assert(anchor == 0);
}

static void test_invalid_inputs(void) {
    pomodoro_model_t model;
    pomo_stats_t stats;
    int64_t anchor = 0;

    /* NULL / 空数据 */
    assert(pomo_blob_decode(NULL, 0, &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_NONE);
    assert_defaults(&model, &stats, anchor);

    /* 魔数错误 */
    pomo_blob_v1_t v1 = make_v1(POMODORO_FOCUS_PAUSED, 1, 7, 100);
    v1.magic = 0x12345678U;
    assert(pomo_blob_decode(&v1, sizeof(v1), &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_NONE);
    assert_defaults(&model, &stats, anchor);

    /* 未知版本 */
    v1 = make_v1(POMODORO_FOCUS_PAUSED, 1, 7, 100);
    v1.version = 3;
    v1.crc32 = pomo_blob_crc32(&v1, offsetof(pomo_blob_v1_t, crc32));
    assert(pomo_blob_decode(&v1, sizeof(v1), &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_NONE);
    assert_defaults(&model, &stats, anchor);

    /* 长度截断 */
    v1 = make_v1(POMODORO_FOCUS_PAUSED, 1, 7, 100);
    assert(pomo_blob_decode(&v1, sizeof(v1) - 1, &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_NONE);
    assert_defaults(&model, &stats, anchor);

    /* v1 CRC 损坏 */
    v1 = make_v1(POMODORO_FOCUS_PAUSED, 1, 7, 100);
    v1.remaining_sec ^= 0xFF;
    assert(pomo_blob_decode(&v1, sizeof(v1), &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_NONE);
    assert_defaults(&model, &stats, anchor);

    /* v2 统计段损坏（CRC 覆盖整包） */
    pomo_blob_v2_t v2;
    fill_model(&model);
    fill_stats(&stats);
    pomo_blob_encode(&v2, &model, &stats, 1770000000LL);
    v2.stats.total_pomos ^= 0xFFU;
    assert(pomo_blob_decode(&v2, sizeof(v2), &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_NONE);
    assert_defaults(&model, &stats, anchor);

    /* v2 锚点损坏 */
    fill_model(&model);
    fill_stats(&stats);
    pomo_blob_encode(&v2, &model, &stats, 1770000000LL);
    v2.anchor_unix ^= 0x1;
    assert(pomo_blob_decode(&v2, sizeof(v2), &model, &stats, &anchor) ==
           POMO_BLOB_DECODE_NONE);
    assert_defaults(&model, &stats, anchor);
}

int main(void) {
    test_v2_roundtrip();
    test_v1_migration();
    test_v1_index_mapping_all();
    test_invalid_inputs();
    puts("pomodoro_blob: all tests passed");
    return 0;
}
