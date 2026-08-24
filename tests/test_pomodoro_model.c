#include "pomodoro_model.h"

#include <assert.h>
#include <stdio.h>

static void test_defaults_and_selection(void) {
    pomodoro_model_t model;
    pomodoro_model_defaults(&model);
    assert(model.state == POMODORO_IDLE);
    assert(pomodoro_model_focus_min(&model) == 25);
    assert(model.remaining_sec == 25 * 60);

    static const uint8_t PRESETS[] = { 5, 10, 15, 20, 25, 30, 45, 60, 90 };
    for (int i = 1; i <= POMODORO_PRESET_COUNT; i++) {
        assert(pomodoro_model_select_duration(&model, 1));
        int idx = (POMODORO_DEFAULT_INDEX + i) % POMODORO_PRESET_COUNT;
        assert(pomodoro_model_focus_min(&model) == PRESETS[idx]);
    }
    assert(pomodoro_model_focus_min(&model) == 25);

    for (int i = 0; i < 4; i++) {
        assert(pomodoro_model_select_duration(&model, -1));
    }
    assert(pomodoro_model_focus_min(&model) == 5);
    assert(pomodoro_model_select_duration(&model, -1));
    assert(pomodoro_model_focus_min(&model) == 90);
}

static void test_break_mapping(void) {
    assert(pomodoro_model_break_min(5) == 5);
    assert(pomodoro_model_break_min(25) == 5);
    assert(pomodoro_model_break_min(30) == 10);
    assert(pomodoro_model_break_min(45) == 10);
    assert(pomodoro_model_break_min(60) == 15);
    assert(pomodoro_model_break_min(90) == 15);
}

static void test_focus_pause_resume_and_completion(void) {
    pomodoro_model_t model;
    pomodoro_model_defaults(&model);
    assert(pomodoro_model_start_focus(&model, 1000));
    assert(model.session_id == 1);
    assert(pomodoro_model_pause(&model, 11000));
    assert(model.remaining_sec == 1490);
    assert(pomodoro_model_resume(&model, 20000));
    assert(pomodoro_model_tick(&model, 1509999) == POMODORO_EVENT_NONE);
    assert(pomodoro_model_tick(&model, 1510000) == POMODORO_EVENT_FOCUS_COMPLETE);
    assert(model.state == POMODORO_REWARD);
    assert(model.pending_break_min == 5);
    assert(pomodoro_model_tick(&model, 1513000) == POMODORO_EVENT_REWARD_FINISHED);
    assert(model.state == POMODORO_BREAK_PROMPT);
}

static void test_abandon_timeout_and_confirm(void) {
    pomodoro_model_t model;
    pomodoro_model_defaults(&model);
    pomodoro_model_start_focus(&model, 0);
    pomodoro_model_pause(&model, 1000);
    assert(pomodoro_model_request_abandon(&model, 2000));
    assert(pomodoro_model_tick(&model, 6999) == POMODORO_EVENT_NONE);
    assert(pomodoro_model_tick(&model, 7000) == POMODORO_EVENT_CONFIRM_TIMEOUT);
    assert(model.state == POMODORO_FOCUS_PAUSED);
    assert(pomodoro_model_request_abandon(&model, 8000));
    assert(pomodoro_model_confirm_abandon(&model));
    assert(model.state == POMODORO_IDLE);
}

static void complete_focus(pomodoro_model_t *model, uint64_t now_ms) {
    assert(pomodoro_model_start_focus(model, now_ms));
    uint64_t end_ms = now_ms + pomodoro_model_focus_min(model) * 60000ULL;
    assert(pomodoro_model_tick(model, end_ms) == POMODORO_EVENT_FOCUS_COMPLETE);
    assert(pomodoro_model_tick(model, end_ms + POMODORO_REWARD_MS) ==
           POMODORO_EVENT_REWARD_FINISHED);
}

static void test_break_cycle_and_long_break(void) {
    pomodoro_model_t model;
    pomodoro_model_defaults(&model);
    uint64_t now_ms = 0;
    for (int i = 0; i < 4; i++) {
        complete_focus(&model, now_ms);
        assert(model.pending_break_min == (i == 3 ? 10 : 5));
        assert(pomodoro_model_skip_break(&model));
        now_ms += 2000000;
    }
    assert(model.pomodoro_round == 0);
}

static void test_long_break_scales_with_focus(void) {
    /* 45 分钟档：普通休息 10，第 4 个番茄翻倍到 20 */
    pomodoro_model_t model;
    pomodoro_model_defaults(&model);
    model.selected_index = 6;
    uint64_t now_ms = 0;
    for (int i = 0; i < 4; i++) {
        complete_focus(&model, now_ms);
        assert(model.pending_break_min == (i == 3 ? 20 : 10));
        assert(pomodoro_model_skip_break(&model));
        now_ms += 4000000;
    }

    /* 90 分钟档：普通休息 15，第 4 个番茄翻倍到 30 */
    pomodoro_model_defaults(&model);
    model.selected_index = 8;
    now_ms = 0;
    for (int i = 0; i < 4; i++) {
        complete_focus(&model, now_ms);
        assert(model.pending_break_min == (i == 3 ? 30 : 15));
        assert(pomodoro_model_skip_break(&model));
        now_ms += 7000000;
    }
}

static void test_break_timer(void) {
    pomodoro_model_t model;
    pomodoro_model_defaults(&model);
    model.state = POMODORO_BREAK_PROMPT;
    model.pending_break_min = 5;
    assert(pomodoro_model_start_break(&model, 1000));
    assert(pomodoro_model_tick(&model, 300999) == POMODORO_EVENT_NONE);
    assert(pomodoro_model_tick(&model, 301000) == POMODORO_EVENT_BREAK_COMPLETE);
    assert(model.state == POMODORO_IDLE);
}

static void test_restore_is_safe(void) {
    pomodoro_model_t model;
    pomodoro_model_defaults(&model);
    model.state = POMODORO_FOCUS_RUNNING;
    model.remaining_sec = 321;
    model.deadline_ms = 999999;
    pomodoro_model_restore(&model);
    assert(model.state == POMODORO_FOCUS_PAUSED);
    assert(model.remaining_sec == 321);
    assert(model.deadline_ms == 0);

    model.state = POMODORO_IDLE;
    model.reward_pending = true;
    pomodoro_model_restore(&model);
    assert(model.state == POMODORO_REWARD);

    /* v1 档位索引越界(旧 3 档表)或非法休息时长时回安全值 */
    pomodoro_model_defaults(&model);
    model.selected_index = 7;
    model.pending_break_min = 45;
    pomodoro_model_restore(&model);
    assert(model.selected_index == 7);
    assert(model.pending_break_min == 15);

    pomodoro_model_defaults(&model);
    model.version = 1;
    pomodoro_model_restore(&model);
    assert(model.selected_index == POMODORO_DEFAULT_INDEX);
    assert(model.pending_break_min == 5);
}

int main(void) {
    test_defaults_and_selection();
    test_break_mapping();
    test_focus_pause_resume_and_completion();
    test_abandon_timeout_and_confirm();
    test_break_cycle_and_long_break();
    test_long_break_scales_with_focus();
    test_break_timer();
    test_restore_is_safe();
    puts("pomodoro_model: all tests passed");
    return 0;
}
