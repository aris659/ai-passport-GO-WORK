#pragma once

/* WiFi 凭据模板：首次构建时若 main/wifi_config.h 不存在，
 * CMake 会自动把本文件复制为 main/wifi_config.h。
 *
 * 填入 2.4GHz WiFi 凭据用于 SNTP 校时，按顺序逐个尝试；SSID 留空的条目跳过，
 * 全部留空则纯离线运行（不初始化 WiFi，闲时屏显示 --:--）。
 *
 * 注意：main/wifi_config.h 已被 .gitignore 忽略，填入真实密码后
 * 不会被提交；切勿把真实凭据写进本模板文件。 */

/* 凭据 1 */
#define POMO_WIFI_SSID_1 ""
#define POMO_WIFI_PASS_1 ""

/* 凭据 2（2.4GHz 频段；ESP32-C3 不支持 5G） */
#define POMO_WIFI_SSID_2 ""
#define POMO_WIFI_PASS_2 ""
