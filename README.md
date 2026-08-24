# WHIPLASH · GO WORK!!

> 黑心老板番茄钟：完成一个番茄 = 吃一鞭。它每两秒在屏幕底部冲你咆哮一句 **GO WORK!!**。

基于 [FoloToy AI Passport](https://github.com/folotoy/ai-passport)（ESP32-C3 口袋设备）官方 BSP 演示改造的纯番茄钟固件：去掉了原版宠物养成系统，重写为一台插电即用的桌面专注工具，视觉走复古街机 Boss 警告风（像素血心 + CRT glitch 抽搐字幕）。

烧录前可先在浏览器打开 [`docs/UI_PREVIEW.html`](docs/UI_PREVIEW.html) 交互式预览全部界面——布局、颜色、动画与固件 1:1。

## 功能

- **专注计时**：9 档时长（5/10/15/20/25/30/45/60/90 分钟，默认 25），休息按时长映射（5/10/15 分钟），每完成第 4 个番茄当次休息翻倍
- **以撒风血心**：专注时血心从满格放血排空，休息时同一颗心从空补满；每秒心跳，暂停即心脏停跳
- **闲时时钟**：自绘像素大数字 + 冒号呼吸闪烁 + 四角瞄准框；显示星期日期、今日鞭数血心行、今日摘要与当前档位
- **GO WORK!! 警告字幕**：粗体右倾像素字 + 硬阴影，每 2 秒发作一次 160ms 的离散抽搐（4 帧步进 + glitch 错位），平时完全静止
- **时间同步**：WiFi SNTP 后台校时（支持两组凭据依次尝试），每小时重同步窗口；断网按"锚点 + 开机时长"推算走时；无 RTC 也能显示日期时间
- **统计**：近 90 天逐日番茄数与分钟数（环形淘汰）+ 生命周期累计；统计页含 7 天柱状图
- **省电**：分场景背光阶梯与超时息屏（闲时 120 秒 / 休息 30 秒），熄灭后任意按键唤醒
- **可靠**：断电重启以暂停态恢复会话；`reward_pending` 标志保证完成瞬间断电不重复计数；NVS 存储版本化（v1 宠物数据自动迁移清空）

## 硬件

FoloToy AI Passport（[官网](https://ai-passport.folotoy.cn/)）：

| 模块 | 型号 |
| --- | --- |
| MCU | ESP32-C3，4MB Flash，无 PSRAM，无 RTC |
| 屏幕 | ST7789P3 240×320 SPI |
| 音频 | ES8311 Codec |
| 电量计 | CW2017 |
| 按键 | UP/DOWN/OK 三键共用 ADC |

## 烧录

### 方式 A：网页烧录（无需装任何工具）

1. 用**数据线**连接设备
2. 打开官方 Web Flasher：<https://ai-passport.folotoy.cn/tools/web-flasher/>
3. 连接设备时选择 `USB JTAG/serial debug unit` 串口
4. 选择 [`releases/whiplash-esp32c3-v1.1.0-merged.bin`](releases/whiplash-esp32c3-v1.1.0-merged.bin)（含引导+分区表的完整镜像，从 0x0 烧写）
5. 写入完成自动重启，开机直接进入 WHIPLASH

> 仓库内固件的 WiFi 凭据为**空**（离线模式，时钟显示 `--:--`）。需要 SNTP 校时请按下文配置 WiFi 后自行编译。

### 方式 B：ESP-IDF 编译烧录

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

### 方式 C：Docker 编译（无需本地安装 ESP-IDF）

```bash
docker pull espressif/idf:v5.5.3
docker run --rm -v "$(pwd):/src" -v pomodoro-build:/work -w /work \
  espressif/idf:v5.5.3 bash /src/tools/docker_build.sh
```

产物回传至 `build/` 与 `releases/`。

## WiFi 配置（SNTP 校时）

`main/wifi_config.h` **不入库**（已 gitignore）：首次构建时 CMake 自动从 [`main/wifi_config.example.h`](main/wifi_config.example.h) 复制生成。填入两组 2.4GHz 凭据即可：

```c
#define POMO_WIFI_SSID_1 "your-ssid"
#define POMO_WIFI_PASS_1 "your-password"
#define POMO_WIFI_SSID_2 ""     // 备用，可留空
#define POMO_WIFI_PASS_2 ""
```

- 开机按顺序逐个尝试（各 15 秒超时）；全部留空则纯离线运行，不初始化 WiFi
- ESP32-C3 仅支持 2.4GHz，5G 频段 SSID 无法连接
- 同步源：ntp.aliyun.com 与 pool.ntp.org，时区固定 UTC+8

> **隐私提醒**：WiFi 凭据会以明文字符串编译进固件二进制。请勿把带真实凭据的自编译 `.bin` 分发给他人或提交到仓库。

## 操作方式

| 状态 | UP 单击 | DOWN 单击 | OK 单击 | 双击 |
| --- | --- | --- | --- | --- |
| 熄灭（任意场景） | 仅唤醒 | 仅唤醒 | 仅唤醒 | 仅唤醒 |
| 闲时 | 上一档 | 下一档 | 开始专注 | UP=统计页 DOWN=静音 |
| 专注中 | 无 | 无 | 暂停 | 无 |
| 专注已暂停 | 发起放弃 | 无 | 继续 | 无 |
| 放弃确认 | 确认放弃 | 取消 | 取消 | 无 |
| 休息提示 | 跳过休息 | 无 | 开始休息 | 无 |
| 休息中 | 无 | 无 | 暂停 | 无 |
| 休息已暂停 | 无 | 无 | 继续 | 无 |
| 统计页 | 无 | 无 | 退出（10 秒自动退出） | 无 |

- 倒计时自然走完才算完成并计入统计；暂停不影响判定；放弃的会话作废不计数
- 长按 OK 保存为暂停并退回 BSP 演示菜单
- 完成番茄播放三连响；双击 DOWN 静音

## 息屏与背光

| 场景 | 亮度阶梯 |
| --- | --- |
| 闲时 / 休息提示 | 100% 保持 15 秒 → 10% 至累计 120 秒 → 熄灭 |
| 专注中 | 100% 保持 5 分钟 → 50% 长期 |
| 专注/休息已暂停 | 100% 保持 60 秒 → 10% 长期 |
| 休息中 | 100% 保持 10 秒 → 10% 至累计 30 秒 → 熄灭 |
| 放弃确认 / 结算动画 / 统计页 | 100% |

## 项目结构

```text
components/bsp/     板级驱动（显示/按键/音频/电池）及公开接口
main/               应用：番茄钟状态机、像素 UI、时间同步、统计与存储
tests/              主机可运行的纯逻辑测试（无需 ESP-IDF）
docs/               PRD、工单、界面预览页与硬件开发指南
tools/              Docker 构建脚本
```

核心模块：`pomodoro_model`（状态机）、`pomodoro_time`（WiFi+SNTP+推算）、`pomodoro_stats`（环形缓冲统计）、`pomodoro_store/blob`（NVS 持久化与版本迁移）、`pomodoro_date`（历法）、`demo_pomodoro`（UI）。

## 测试

纯逻辑测试在主机直接跑（模型 / 日期 / 统计 / 存储 blob / 像素数学共 5 套）：

```bash
bash tests/run_all_tests.sh
```

显示、音频、电池和实体按键需在真机验收。

## 致谢与许可

- 本项目派生自 [folotoy/ai-passport](https://github.com/folotoy/ai-passport)（MIT License, Copyright (c) 2026 FoloToy），BSP 驱动与硬件文档来自上游
- 视觉灵感：《以撒的结合》血心、《爆裂鼓手》Whiplash
- [MIT License](LICENSE)
