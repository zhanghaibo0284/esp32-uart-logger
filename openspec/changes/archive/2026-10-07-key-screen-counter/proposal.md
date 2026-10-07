# Proposal

## Why

现场长期部署需要两点保障：一是屏幕常亮既干扰现场又无谓消耗，且设备安装位置可能不易通过触摸操作，需要物理按键可靠地开关屏幕并在无人操作时自动息屏；二是收发统计计数器为 32 位，在长期不重启、高频通信场景下存在溢出归零的隐患，影响通信量统计的连续性。本次变更补齐屏幕电源管理与计数溢出保护。

## What Changes

- 支持物理按键长按：K1 长按 2 秒关闭屏幕，K2 长按 2 秒点亮屏幕；按键检测带消抖处理，短按行为保持不变。
- 屏幕无操作 5 分钟自动关闭；任何触摸或按键操作重置计时；该功能可通过配置项启用/禁用，默认开启。
- 屏幕状态切换提供明确反馈（指示灯与日志）。
- 收发统计计数器扩展为 64 位，任何现实通信速率与部署周期下均不会溢出，且计数与统计路径不增加额外开销。

## Capabilities

### New Capabilities

- `screen-power`：物理按键长按点亮/关闭屏幕、无操作自动息屏、活动计时重置与切换反馈。
- `traffic-statistics`：收发字节/帧/丢弃计数的溢出保护与持续准确统计。

### Modified Capabilities

- `config-management`：新增屏幕自动息屏开关配置项 `screen_auto_off`（默认开启），纳入统一配置的校验、同步、备份体系；配置存储格式升级并兼容旧版本。

## Impact

- 新增模块：`main/app_screen.[ch]`。
- 修改模块：`main/app_settings.[ch]`（新增字段，NVS blob v3 兼容 v2）、`main/app_config.c`（快照/导入/变化检测）、`main/app_logger.[ch]`（计数 64 位）、`main/app_bridge.h`（计数 64 位）、`main/app_httpd.c`（计数格式）、`main/lvgl_port.c`（触摸活动钩子）、`main/app_ui.c`（短按异步化）、`main/main.c`、`main/CMakeLists.txt`。
- 不改变 BLE/WiFi 初始化顺序；新增任务与大缓冲均不占用额外内部内存（缓冲位于 PSRAM）。
