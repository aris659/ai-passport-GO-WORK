#include "pomodoro_model.h"

#include <stddef.h>
#include <string.h>

static const uint8_t FOCUS_MINUTES[POMODORO_PRESET_COUNT] = {
    5, 10, 15, 20, 25, 30, 45, 60, 90
};

static uint32_t seconds_until(uint64_t deadline_ms, uint64_t now_ms) {
    if (now_ms >= deadline_ms) return 0;
    return (uint32_t)((deadline_ms - now_ms + 999) / 1000);
}

static void update_running_remaining(pomodoro_model_t *model, uint64_t now_ms) {
    if (model->state == POMODORO_FOCUS_RUNNING) {
        model->remaining_sec = seconds_until(model->deadline_ms, now_ms);
    } else if (model->state == POMODORO_BREAK_RUNNING) {
        model->break_remaining_sec = seconds_until(model->deadline_ms, now_ms);
    }
}

static bool break_min_valid(uint8_t min) {
    return min == 5 || min == 10 || min == 15 || min == 20 || min == 30;
}

void pomodoro_model_defaults(pomodoro_model_t *model) {
    if (!model) return;
    memset(model, 0, sizeof(*model));
    model->version = POMODORO_MODEL_VERSION;
    model->state = POMODORO_IDLE;
    model->selected_index = POMODORO_DEFAULT_INDEX;
    model->remaining_sec = FOCUS_MINUTES[POMODORO_DEFAULT_INDEX] * 60U;
    model->pending_break_min =
        pomodoro_model_break_min(FOCUS_MINUTES[POMODORO_DEFAULT_INDEX]);
}

void pomodoro_model_restore(pomodoro_model_t *model) {
    if (!model) return;

    if (model->version != POMODORO_MODEL_VERSION ||
        model->selected_index >= POMODORO_PRESET_COUNT ||
        model->state > POMODORO_BREAK_PAUSED) {
        pomodoro_model_defaults(model);
        return;
    }

    model->pomodoro_round %= 4;
    model->deadline_ms = 0;
    model->confirm_deadline_ms = 0;
    model->reward_deadline_ms = 0;

    if (model->reward_pending) {
        model->state = POMODORO_REWARD;
    } else if (model->state == POMODORO_FOCUS_RUNNING ||
               model->state == POMODORO_ABANDON_CONFIRM) {
        model->state = POMODORO_FOCUS_PAUSED;
    } else if (model->state == POMODORO_BREAK_RUNNING) {
        model->state = POMODORO_BREAK_PAUSED;
    } else if (model->state == POMODORO_REWARD) {
        model->state = POMODORO_BREAK_PROMPT;
    }

    if (model->remaining_sec == 0 &&
        (model->state == POMODORO_IDLE || model->state == POMODORO_FOCUS_PAUSED)) {
        model->remaining_sec = pomodoro_model_focus_min(model) * 60U;
    }
    if (!break_min_valid(model->pending_break_min)) {
        model->pending_break_min = pomodoro_model_break_min(
            pomodoro_model_focus_min(model));
    }
}

uint8_t pomodoro_model_focus_min(const pomodoro_model_t *model) {
    if (!model || model->selected_index >= POMODORO_PRESET_COUNT) {
        return FOCUS_MINUTES[POMODORO_DEFAULT_INDEX];
    }
    return FOCUS_MINUTES[model->selected_index];
}

uint8_t pomodoro_model_break_min(uint8_t focus_min) {
    if (focus_min >= 60) return 15;
    if (focus_min >= 30) return 10;
    return 5;
}

uint8_t pomodoro_model_index_of(uint8_t focus_min) {
    for (uint8_t i = 0; i < POMODORO_PRESET_COUNT; i++) {
        if (FOCUS_MINUTES[i] == focus_min) return i;
    }
    return POMODORO_DEFAULT_INDEX;
}

bool pomodoro_model_select_duration(pomodoro_model_t *model, int direction) {
    if (!model || model->state != POMODORO_IDLE || direction == 0) return false;
    int index = (int)model->selected_index + (direction > 0 ? 1 : -1);
    if (index < 0) index = POMODORO_PRESET_COUNT - 1;
    if (index >= POMODORO_PRESET_COUNT) index = 0;
    model->selected_index = (uint8_t)index;
    model->remaining_sec = pomodoro_model_focus_min(model) * 60U;
    return true;
}

bool pomodoro_model_start_focus(pomodoro_model_t *model, uint64_t now_ms) {
    if (!model || model->state != POMODORO_IDLE) return false;
    model->remaining_sec = pomodoro_model_focus_min(model) * 60U;
    model->session_id++;
    model->deadline_ms = now_ms + model->remaining_sec * 1000ULL;
    model->state = POMODORO_FOCUS_RUNNING;
    return true;
}

bool pomodoro_model_pause(pomodoro_model_t *model, uint64_t now_ms) {
    if (!model) return false;
    update_running_remaining(model, now_ms);
    if (model->state == POMODORO_FOCUS_RUNNING) {
        model->state = POMODORO_FOCUS_PAUSED;
        model->deadline_ms = 0;
        return true;
    }
    if (model->state == POMODORO_BREAK_RUNNING) {
        model->state = POMODORO_BREAK_PAUSED;
        model->deadline_ms = 0;
        return true;
    }
    return false;
}

bool pomodoro_model_resume(pomodoro_model_t *model, uint64_t now_ms) {
    if (!model) return false;
    if (model->state == POMODORO_FOCUS_PAUSED && model->remaining_sec > 0) {
        model->state = POMODORO_FOCUS_RUNNING;
        model->deadline_ms = now_ms + model->remaining_sec * 1000ULL;
        return true;
    }
    if (model->state == POMODORO_BREAK_PAUSED && model->break_remaining_sec > 0) {
        model->state = POMODORO_BREAK_RUNNING;
        model->deadline_ms = now_ms + model->break_remaining_sec * 1000ULL;
        return true;
    }
    return false;
}

bool pomodoro_model_request_abandon(pomodoro_model_t *model, uint64_t now_ms) {
    if (!model || model->state != POMODORO_FOCUS_PAUSED) return false;
    model->state = POMODORO_ABANDON_CONFIRM;
    model->confirm_deadline_ms = now_ms + POMODORO_CONFIRM_MS;
    return true;
}

bool pomodoro_model_cancel_abandon(pomodoro_model_t *model) {
    if (!model || model->state != POMODORO_ABANDON_CONFIRM) return false;
    model->state = POMODORO_FOCUS_PAUSED;
    model->confirm_deadline_ms = 0;
    return true;
}

bool pomodoro_model_confirm_abandon(pomodoro_model_t *model) {
    if (!model || model->state != POMODORO_ABANDON_CONFIRM) return false;
    model->state = POMODORO_IDLE;
    model->remaining_sec = pomodoro_model_focus_min(model) * 60U;
    model->confirm_deadline_ms = 0;
    return true;
}

bool pomodoro_model_start_break(pomodoro_model_t *model, uint64_t now_ms) {
    if (!model || model->state != POMODORO_BREAK_PROMPT) return false;
    if (!break_min_valid(model->pending_break_min)) {
        model->pending_break_min = pomodoro_model_break_min(
            pomodoro_model_focus_min(model));
    }
    model->break_remaining_sec = model->pending_break_min * 60U;
    model->deadline_ms = now_ms + model->break_remaining_sec * 1000ULL;
    model->state = POMODORO_BREAK_RUNNING;
    return true;
}

bool pomodoro_model_skip_break(pomodoro_model_t *model) {
    if (!model || model->state != POMODORO_BREAK_PROMPT) return false;
    model->state = POMODORO_IDLE;
    model->break_remaining_sec = 0;
    model->remaining_sec = pomodoro_model_focus_min(model) * 60U;
    return true;
}

pomodoro_event_t pomodoro_model_tick(pomodoro_model_t *model, uint64_t now_ms) {
    if (!model) return POMODORO_EVENT_NONE;

    update_running_remaining(model, now_ms);
    if (model->state == POMODORO_FOCUS_RUNNING && model->remaining_sec == 0) {
        model->pending_break_min = pomodoro_model_break_min(
            pomodoro_model_focus_min(model));
        if (model->pomodoro_round == 3) model->pending_break_min *= 2;
        model->pomodoro_round = (model->pomodoro_round + 1) % 4;
        model->reward_pending = true;
        model->state = POMODORO_REWARD;
        model->deadline_ms = 0;
        model->reward_deadline_ms = now_ms + POMODORO_REWARD_MS;
        return POMODORO_EVENT_FOCUS_COMPLETE;
    }

    if (model->state == POMODORO_REWARD) {
        if (model->reward_deadline_ms == 0) {
            model->reward_deadline_ms = now_ms + POMODORO_REWARD_MS;
        } else if (now_ms >= model->reward_deadline_ms) {
            model->reward_pending = false;
            model->state = POMODORO_BREAK_PROMPT;
            model->reward_deadline_ms = 0;
            return POMODORO_EVENT_REWARD_FINISHED;
        }
    }

    if (model->state == POMODORO_BREAK_RUNNING && model->break_remaining_sec == 0) {
        model->state = POMODORO_IDLE;
        model->remaining_sec = pomodoro_model_focus_min(model) * 60U;
        model->deadline_ms = 0;
        return POMODORO_EVENT_BREAK_COMPLETE;
    }

    if (model->state == POMODORO_ABANDON_CONFIRM &&
        now_ms >= model->confirm_deadline_ms) {
        model->state = POMODORO_FOCUS_PAUSED;
        model->confirm_deadline_ms = 0;
        return POMODORO_EVENT_CONFIRM_TIMEOUT;
    }
    return POMODORO_EVENT_NONE;
}
