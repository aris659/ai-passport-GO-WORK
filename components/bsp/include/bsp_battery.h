// components/bsp/include/bsp_battery.h
// CellWise CW2017 电量计:I2C 0x63,与 ES8311 共用总线。
// 芯片自带 Li-Poly profile 按 OCV 估算 SOC;CW2017 数据手册(CW2017-DS V1.1)
// 的寄存器表里没有 CW2015 那种从 0x10 开始的 BATINFO 主机可写区,
// 因此主机侧不存在"写 profile"步骤,也读不回 profile。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// 一次诊断快照:寄存器原始值 + 换算值(用于启动日志与 Battery demo 页)。
typedef struct {
    bool     online;      // 所有诊断寄存器读取成功(芯片应答)
    uint8_t  version;     // 0x00,CW2017 应答 0xA0
    uint8_t  config;      // 0x08:0xF0=睡眠(上电默认) / 0x30=复位挂起 / 0x00=正常
    uint8_t  soc_alert;   // 0x0B:SOC 告警阈值,默认 0x14
    uint16_t soc_raw;     // 0x04/0x05,高字节=整数百分比
    uint16_t vcell_raw;   // 0x02/0x03,14bit
    int      voltage_mv;  // vcell_raw × 312.5µV
    int      soc;         // 0..100;芯片未就绪(>100)或读取失败为 -1
} bsp_battery_diag_t;

// 初始化:探测 VERSION → 打印诊断日志 → 按数据手册执行 0x30→0x00 唤醒序列。
// 返回 ESP_OK 仅代表芯片在线应答;SOC 首次计算需要数秒,就绪判定交给上层轮询。
// 芯片不应答返回 ESP_ERR_NOT_FOUND;唤醒寄存器写失败返回 ESP_FAIL(可重试)。
esp_err_t bsp_battery_init(void);

// 运行时恢复:重走 0x30→0x00 序列,芯片复位并重新计算 SOC。
// 调用方必须自带 cooldown,禁止高频复位芯片。
esp_err_t bsp_battery_restart(void);

// 读取诊断快照。false = I2C 无应答(未 init 或芯片离线)。
bool bsp_battery_diag(bsp_battery_diag_t *out);

// 剩余电量百分比 0..100;读失败或未就绪返回 -1。
// 注意:raw=0x0000 时本函数返回 0——那是芯片上电默认值,不代表真实 0%;
// 就绪判定请用 bsp_battery_soc_raw() + 调用方的 plausibility 逻辑。
int bsp_battery_soc(void);

// 完整 16bit SOC raw(0x04 高字节 | 0x05 低字节,低字节 = 1/256 %)。
// 读取成功返回 true。raw==0x0000 是上电默认值,CW2017-DS V1.1 规定
// SOC 上电默认 0x00/0x00;判定可信性必须结合电压。
bool bsp_battery_soc_raw(uint16_t *raw);

// 电池电压 mV;读失败返回 -1。
int bsp_battery_mv(void);
