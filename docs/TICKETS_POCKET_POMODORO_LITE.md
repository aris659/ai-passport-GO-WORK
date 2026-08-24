# 口袋番茄钟 · 纯净版 工单拆分

依据 `docs/PRD_POCKET_POMODORO_LITE.md` 拆分。工单按依赖顺序编号，每个工单完成后应通过其"完成标准"。

## 执行状态（2026-08-24）

| 工单 | 状态 | 验证 |
| --- | --- | --- |
| T1 模型层 9 档与休息映射 | 完成 | 主机测试 test_model 通过 |
| T2 日期与统计纯逻辑 | 完成 | 主机测试 test_date / test_stats 通过 |
| T3 存储层 v2 与 v1 迁移 | 完成 | 主机测试 test_blob（含 v1 迁移/回滚用例）通过 |
| T4 时间同步模块 | 完成 | 主机可测部分已覆盖；真机 SNTP 校时待上机验证 |
| T5 UI 重构 | 完成 | 代码就绪；各屏目检待上机 |
| T6 息屏与背光策略 | 完成 | 代码就绪；场景时序待上机 |
| T7 统计接线与记账 | 完成 | 代码就绪；完成番茄后数值一致性待上机 |
| T8 分区表与构建收尾 | 完成 | factory 3MB 分区适配 WiFi 固件 |
| T9 测试、README 与收尾 | 完成 | 5 套主机测试全绿（`bash tests/run_all_tests.sh`）；README 已同步 |

一键回归：`bash tests/run_all_tests.sh`（无 ESP-IDF 依赖；Windows 主机可用 `docker run --rm -v <项目目录>:/src gcc:14 bash /src/tests/run_all_tests.sh`）。

## Release Hardening（2026-08-25，v1.2.0）

第一版公开发布前的收尾轮，只修 bug 不扩需求：

| 项 | 内容 | 状态 |
| --- | --- | --- |
| H1 统计页 weekday 对齐 | 近 7 天柱序是 `today-6..today` 滚动窗口，星期标签从固定 `MTWTFSS` 改为 `pomo_weekday_letters()` 按实际日期生成 | 完成（test_date 新增周一周三周日用例） |
| H2 闲时 TODAY 摘要溢出 | 新增 `pomo_summary_fit()`：完整文案@scale2 → 紧凑文案@scale2 → 紧凑文案@scale1 逐级退化，安全区 230px | 完成（test_date 新增拟合用例） |
| H3 SNTP 重同步周期 | 正常同步成功后的重同步 1 小时 → 6 小时（省电）；失败重试仍为 30 分钟 | 完成 |
| H4 WiFi/SNTP soft-fail | `pomodoro_time.c` 运行期全部 `ESP_ERROR_CHECK` 改为记日志+跳过本窗口：网络故障不再 abort 番茄钟本体 | 完成 |
| H5 timer tick 与真实时间解耦 | 电池轮询等从 tick 计数改为 `now_ms()` 差值（GO WORK 动画临时切 40ms 周期不再失真）；GO WORK 计时变量统一 `uint64_t` | 完成 |
| H6 文档与预览同步 | README / PRD / UI_PREVIEW 同步 6 小时重同步与上述行为；发布物升级 v1.2.0 | 完成 |

## T1 模型层：9 档时长与休息映射

- 文件：`main/pomodoro_model.c`、`main/pomodoro_model.h`、`tests/test_pomodoro_model.c`
- 内容：
  - `FOCUS_MINUTES` 改为 {5,10,15,20,25,30,45,60,90}，默认档 index 5 对应 25 分钟
  - 新增 `break_min_for(focus_min)` 映射（≤25→5、30-45→10、60-90→15）；专注完成时计算休息时长，`pomodoro_round==3` 时翻倍
  - 删除 `CAT_THRESHOLDS`、`pomodoro_model_cat_stage()`、`pomodoro_model_growth()`；模型移除 `completed_sessions`、`completed_focus_min` 字段（移交统计模块）
  - `POMODORO_MODEL_VERSION` 升为 2；`pending_break_min` 恢复校验改为合法集合 {5,10,15,20,30}
- 完成标准：更新后的主机测试全部通过（含新增档位循环、映射、翻倍用例）

## T2 日期与统计纯逻辑模块

- 新文件：`main/pomodoro_date.c/.h`、`main/pomodoro_stats.c/.h`；新测试：`tests/test_pomodoro_date.c`、`tests/test_pomodoro_stats.c`
- 内容：
  - `pomodoro_date`：UTC+8 固定偏移；unix 秒 <-> epoch 天数（uint16）互转、年月日分解、星期（周一=0）、周起始日
  - `pomodoro_stats`：`pomo_stats_t`（90 日环形桶 + 双累计）；`record`（当日累加/跨日新桶/满环淘汰/无日期只计累计）、`day` 查询、`last7` 查询、`week` 周聚合；纯逻辑不依赖 ESP-IDF
- 完成标准：两个新测试文件全部通过

## T3 存储层 v2 与 v1 迁移

- 文件：`main/pomodoro_store.c`、`main/pomodoro_store.h`
- 内容：
  - blob 升 v2：新增 `anchor_unix`（int64）、统计字段（双累计 + 环形桶 head/count + 90 桶）；移除宠物相关字段
  - v1 blob 读取与迁移：继承可映射字段；selected_index 按分钟查新表；累计继承旧 completed 值；锚点置 0；逐日桶清空
  - API 变为 `pomodoro_store_init(model, stats, &anchor)` / `pomodoro_store_request_save(model, stats, anchor)`
- 完成标准：逻辑自查（迁移路径、CRC 失败回默认）+ 编译通过

## T4 时间同步模块

- 新文件：`main/pomodoro_time.c/.h`、`main/wifi_config.h`；修改：`main/CMakeLists.txt`
- 内容：
  - 三态 `pomo_time_status_t`（NONE/ESTIMATE/SYNCED）；`pomodoro_time_init(anchor)` 载入锚点并启动 WiFi 任务；`pomodoro_time_now_unix()` 在 SYNCED 时读系统时钟、否则 锚点+开机时长
  - WiFi 任务：凭据为空直接退出；STA 连接（单凭据 15 秒超时）→ SNTP（双服务器、60 秒同步间隔、同步回调置位）→ 同步后关 WiFi；每 6 小时重同步一次；连续失败每 30 分钟重试；全程 soft-fail 不 abort
  - CMakeLists：REQUIRES 增加 `esp_wifi esp_netif esp_event lwip`
- 完成标准：编译通过；上机填凭据后 30 秒内 SYNCED（真机项）

## T5 UI 重构：闲时时钟与专注界面

- 文件：`main/demo_pomodoro.c`、`sdkconfig.defaults`
- 内容：
  - 删除：猫/宠物卡片/成长条/升级音效相关全部代码与对象；大号番茄改为 40px 角落装饰
  - 闲时屏：日期行 + 48px 时钟 + 未校准圆点 + 今日摘要 + 档位行 + 操作提示；每分钟刷新时钟/日期
  - 专注/休息屏：环心 48px MM:SS 倒计时（原番茄位置），刻度环/顶栏/底部标签沿用
  - 结算屏：+1 与 POMO DONE、爱心闪烁、三连响
  - 统计页：标题 + 今日摘要 + 7 天柱状图（今日高亮）+ 周/累计两行；双击 UP 进入、OK 退出、10 秒自动退出
  - sdkconfig 启用 `CONFIG_LV_FONT_MONTSERRAT_28/48`
- 完成标准：编译通过；上机目检各屏（真机项）

## T6 息屏与背光策略

- 文件：`main/demo_pomodoro.c`
- 内容：
  - 背光状态机：按 PRD F5 场景表计算目标亮度，200ms 周期评估，变化时调用 `bsp_display_backlight`
  - 活动时间与场景计时跟踪；熄灭态首键仅唤醒不触发动作；FOCUS_COMPLETE/BREAK_COMPLETE 强制 100%
- 完成标准：编译通过；上机验证各场景时序（真机项）

## T7 统计接线与完成记账

- 文件：`main/demo_pomodoro.c`
- 内容：
  - FOCUS_COMPLETE 时以当前日期调 `pomo_stats_record`（无时间基准时只计累计）；持久化携带统计与锚点
  - 闲时今日摘要、统计页数据接线；时间状态轮询（状态变化时持久化锚点）
  - 提示音：完成三连响、休息结束三连响
- 完成标准：编译通过；完成一个番茄后摘要/统计页/断电重启数值一致（真机项）

## T8 分区表与构建收尾

- 新文件：`partitions.csv`；修改：`sdkconfig.defaults`
- 内容：
  - 自定义分区表：nvs 24K + phy 4K + factory 3MB（4MB flash 内，兼容 8MB 批次）
  - sdkconfig：`CONFIG_PARTITION_TABLE_CUSTOM=y` 指向该表
- 完成标准：编译通过且 app 分区可容纳 WiFi 版固件

## T9 测试、README 与总收尾

- 文件：`tests/*`、`README.md`
- 内容：
  - 主机运行全部测试并修复
  - README：新交互表、WiFi 配置说明（wifi_config.h）、统计/息屏行为、构建测试命令更新
- 完成标准：主机测试全绿；文档与实现一致
