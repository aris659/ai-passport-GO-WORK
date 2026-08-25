// components/bsp/src/bsp_battery.c
// 移植自 trae_card/components/platform/platform_esp32/src/battery_cw2017.c
// (去掉电池 profile 写入部分:CW2017 使用芯片自带 Li-Poly profile;
//  其数据手册寄存器表没有 CW2015 的 BATINFO 主机可写区)
//
// 寄存器事实来源:CellWise CW2017-DS V1.1
//   VERSION   0x00  R    默认 0xA0
//   VCELL     0x02  R    14bit,V(µV) = raw × 312.5
//   SOC       0x04  R    16bit,% = 高字节 + 低字节/256
//   CONFIG    0x08  R/W  上电默认 0xF0 = 睡眠模式
//   SOC_ALERT 0x0B  R/W  SOC 告警阈值,默认 0x14
// 上电后芯片停在睡眠模式,数据手册规定必须两步唤醒:
//   ① 写 0x30 到 0x08(清睡眠位) ② 写 0x00 到 0x08(清复位位,芯片复位固件
//   并基于最新电池状态重算 SOC)。只写一次 0x00 不是数据手册流程。
#include <string.h>

#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_batt";

#define CW_REG_VERSION   0x00
#define CW_REG_VCELL_H   0x02
#define CW_REG_SOC_H     0x04
#define CW_REG_CONFIG    0x08
#define CW_REG_SOC_ALERT 0x0B

static i2c_master_dev_handle_t s_dev;
static bool s_awake;   // 已成功执行过唤醒序列

static int cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

static int cw_vcell_mv(const uint8_t b[2]) {
    uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
    return (int)((raw * 3125) / 10000);                     // raw × 312.5µV → mV
}

// 数据手册唤醒/复位序列:0x30(清睡眠位) → 0x00(清复位位)。
static esp_err_t cw_wake(void) {
    if (cw_write(CW_REG_CONFIG, 0x30) != 0) {
        ESP_LOGE(TAG, "CW2017 写 CONFIG=0x30 失败(清睡眠位)");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));   // 数据手册未规定间隔;保守留 10ms
    if (cw_write(CW_REG_CONFIG, 0x00) != 0) {
        ESP_LOGE(TAG, "CW2017 写 CONFIG=0x00 失败(清复位位)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 读全部诊断寄存器;任何一处 NACK 都返回 false。
static bool cw_read_diag(bsp_battery_diag_t *d) {
    uint8_t vcell[2] = { 0 }, soc[2] = { 0 };
    if (cw_read(CW_REG_VERSION, &d->version, 1) != 0)     return false;
    if (cw_read(CW_REG_VCELL_H, vcell, 2) != 0)           return false;
    if (cw_read(CW_REG_SOC_H, soc, 2) != 0)               return false;
    if (cw_read(CW_REG_CONFIG, &d->config, 1) != 0)       return false;
    if (cw_read(CW_REG_SOC_ALERT, &d->soc_alert, 1) != 0) return false;
    d->vcell_raw  = (uint16_t)(((uint32_t)vcell[0] << 8 | vcell[1]) & 0x3FFF);
    d->soc_raw    = (uint16_t)((uint32_t)soc[0] << 8 | soc[1]);
    d->voltage_mv = cw_vcell_mv(vcell);
    d->soc        = soc[0] <= 100 ? soc[0] : -1;
    d->online     = true;
    return true;
}

static void cw_log_diag(const char *phase) {
    bsp_battery_diag_t d = { 0 };
    if (!cw_read_diag(&d)) {
        ESP_LOGW(TAG, "CW2017 diagnostic(%s): 寄存器读取失败", phase);
        return;
    }
    ESP_LOGI(TAG, "CW2017 diagnostic(%s):", phase);
    ESP_LOGI(TAG, "  version=0x%02X mode=0x%02X soc_alert=0x%02X",
             d.version, d.config, d.soc_alert);
    ESP_LOGI(TAG, "  soc_raw=0x%04X integer=%d vcell_raw=0x%04X voltage=%dmV",
             d.soc_raw, d.soc, d.vcell_raw, d.voltage_mv);
}

esp_err_t bsp_battery_init(void) {
    if (s_dev && s_awake) return ESP_OK;

    if (!s_dev) {
        esp_err_t e = bsp_i2c_init();
        if (e != ESP_OK) return e;

        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = BSP_I2C_CW2017_ADDR,
            .scl_speed_hz    = 100000,
        };
        e = i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "添加 I2C 设备失败: %s", esp_err_to_name(e));
            return e;
        }
    }

    uint8_t ver = 0;
    if (cw_read(CW_REG_VERSION, &ver, 1) != 0) {
        ESP_LOGW(TAG, "CW2017 未应答 —— 用 bsp_i2c_scan() 确认 0x%02X 是否在线;"
                      "无电量计的板子可忽略本项", BSP_I2C_CW2017_ADDR);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "检测到 CW2017 VERSION=0x%02X", ver);
    cw_log_diag("before-wake");    // 上电默认应见 mode=0xF0(睡眠)

    if (cw_wake() != ESP_OK) {
        s_awake = false;
        return ESP_FAIL;           // 芯片应答过但唤醒写失败;保留 s_dev 供下次只重试唤醒
    }
    s_awake = true;
    vTaskDelay(pdMS_TO_TICKS(100));   // 等首次 SOC 计算起步;就绪判定交给上层轮询

    bsp_battery_diag_t d = { 0 };
    if (cw_read_diag(&d)) {
        if (d.soc >= 0) {
            ESP_LOGI(TAG, "CW2017 online, voltage=%dmV, SOC=%d%%", d.voltage_mv, d.soc);
        } else {
            ESP_LOGW(TAG, "CW2017 online, voltage=%dmV, SOC not ready (raw=0x%04X)",
                     d.voltage_mv, d.soc_raw);
        }
    }
    return ESP_OK;
}

esp_err_t bsp_battery_restart(void) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    esp_err_t e = cw_wake();
    if (e == ESP_OK) {
        s_awake = true;
        ESP_LOGI(TAG, "CW2017 restart(0x30→0x00) 完成,SOC 将重新计算");
    }
    return e;
}

bool bsp_battery_diag(bsp_battery_diag_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->soc = -1;
    return s_dev && cw_read_diag(out);
}

int bsp_battery_soc(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_SOC_H, b, 2) != 0) return -1;
    int soc = b[0];                       // 高字节即整数百分比
    if (soc > 100) return -1;             // 芯片未就绪时可能读到 0xFF
    return soc;
}

bool bsp_battery_soc_raw(uint16_t *raw) {
    if (!raw) return false;
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_SOC_H, b, 2) != 0) return false;
    *raw = (uint16_t)(((uint32_t)b[0] << 8) | b[1]);
    return true;
}

int bsp_battery_mv(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_VCELL_H, b, 2) != 0) return -1;
    return cw_vcell_mv(b);
}
