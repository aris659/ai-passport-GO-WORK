#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    POMO_TIME_NONE = 0,  /* 无任何时间基准 */
    POMO_TIME_ESTIMATE,  /* 锚点 + 开机时长推算 */
    POMO_TIME_SYNCED,    /* SNTP 已同步，读系统时钟 */
} pomo_time_status_t;

/* WiFi 连接结果（供配网结果反馈 / 状态显示） */
typedef enum {
    POMO_WIFI_STATE_OFFLINE = 0,   /* 无凭据，未尝试 */
    POMO_WIFI_STATE_CONNECTING,    /* 正在连接/同步 */
    POMO_WIFI_STATE_CONNECTED,     /* SNTP 已同步 */
    POMO_WIFI_STATE_FAILED,        /* 本轮所有凭据均失败 */
} pomo_wifi_state_t;

/* 载入持久化的锚点并启动后台校时任务；可重复调用（只启动一次）。
 * 凭据来源优先级：NVS 用户配置（SoftAP 配网）> wifi_config.h 编译期；
 * 两者皆无时自动启动一次 SoftAP 配网会话。 */
void pomodoro_time_init(int64_t anchor_unix);

pomo_time_status_t pomodoro_time_status(void);

/* 当前最优 unix 时间(UTC)；无基准时返回 0。 */
int64_t pomodoro_time_now_unix(void);

/* 当前本地日期(epoch 天数)；无基准时返回 POMO_NO_DATE。 */
uint16_t pomodoro_time_today(void);

pomo_wifi_state_t pomodoro_time_wifi_state(void);

/* ---- WiFi 驱动仲裁（配网会话与校时任务互斥） ----
 * suspend：阻塞获取驱动锁（timeout_ms 超时返回 false），
 *          成功后到 resume 前校时任务不会触碰 esp_wifi_*。
 * resume：释放驱动锁，恢复 STA 模式由调用方负责。 */
bool pomodoro_time_wifi_suspend(uint32_t timeout_ms);
void pomodoro_time_wifi_resume(void);

/* 凭据可能已变化（配网保存后调用）：唤醒校时任务立即重读并尝试。 */
void pomodoro_time_reload_credentials(void);
