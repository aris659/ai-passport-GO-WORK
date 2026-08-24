#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pomodoro_model.h"
#include "pomodoro_stats.h"

bool pomodoro_store_init(pomodoro_model_t *model, pomo_stats_t *stats,
                         int64_t *anchor_unix);
void pomodoro_store_request_save(const pomodoro_model_t *model,
                                 const pomo_stats_t *stats,
                                 int64_t anchor_unix);
bool pomodoro_store_has_error(void);
