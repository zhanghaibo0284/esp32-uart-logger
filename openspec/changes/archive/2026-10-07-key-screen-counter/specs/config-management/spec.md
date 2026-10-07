# Spec Delta

## ADDED Requirements

### Requirement: 屏幕自动息屏配置项
系统 SHALL 在统一配置中提供屏幕自动息屏开关项 `screen_auto_off`，默认值 MUST 为开启；该项 MUST 随其余配置一起被校验、持久化、导出到 config.json、备份与恢复，并在启动时自动加载生效。

#### Scenario: 默认开启并写入配置
- **WHEN** 设备以初始配置启动并导出 config.json
- **THEN** 配置中包含 `screen_auto_off` 且值为 true，屏幕按该项执行自动息屏

#### Scenario: 修改后同步与加载
- **WHEN** 用户将该项改为禁用（或重新开启）
- **THEN** config.json 自动反映新值，重启加载后设备按新配置执行

#### Scenario: 非法值被拒绝
- **WHEN** 导入配置中该项不是有效的布尔值
- **THEN** 系统拒绝该配置，当前运行配置不受影响

### Requirement: 旧配置兼容升级
系统 SHALL 在配置存储格式升级后继续接受旧版本配置：旧版本中的配置参数 MUST 被完整保留，新增的 `screen_auto_off` 在旧版本中缺失时 MUST 取默认开启，不得因格式升级丢弃既有配置。

#### Scenario: 旧版本配置启动
- **WHEN** 设备中为升级前格式的配置并在升级后启动
- **THEN** 原有参数全部保留，屏幕自动息屏按默认开启执行
