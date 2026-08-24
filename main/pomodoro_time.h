#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    POMO_TIME_NONE = 0,  /* 无任何时间基准 */
    POMO_TIME_ESTIMATE,  /* 锚点 + 开机时长推算 */
    POMO_TIME_SYNCED,    /* SNTP 已同步，读系统时钟 */
} pomo_time_status_t;

/* 载入持久化的锚点并启动后台校时任务；可重复调用（只启动一次）。 */
void pomodoro_time_init(int64_t anchor_unix);

pomo_time_status_t pomodoro_time_status(void);

/* 当前最优 unix 时间(UTC)；无基准时返回 0。 */
int64_t pomodoro_time_now_unix(void);

/* 当前本地日期(epoch 天数)；无基准时返回 POMO_NO_DATE。 */
uint16_t pomodoro_time_today(void);
