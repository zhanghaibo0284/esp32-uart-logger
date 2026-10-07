# Spec Delta

## Purpose

以 TF 卡根目录的单一 JSON 配置文件统一管理设备全部参数，支持启动加载、屏幕与 Web 修改的自动双向同步、校验以及备份恢复，便于现场配置的迁移与保全。

## ADDED Requirements

### Requirement: 启动自动加载
系统 SHALL 在启动时读取 TF 卡根目录 config.json 并应用其中参数；文件不存在时 MUST 以当前配置正常启动，文件内容无效时 MUST 拒绝该文件并保持现有配置、随后以有效配置修复文件。

#### Scenario: 首次启动无配置文件
- **WHEN** 设备以空 TF 卡启动
- **THEN** 系统按内置默认参数正常启动，并在首次配置导出时创建 config.json

#### Scenario: 配置文件损坏
- **WHEN** config.json 内容无法解析或校验失败
- **THEN** 系统保留当前运行配置，不发生异常或重启，并用有效配置重写该文件

### Requirement: 修改自动双向同步
系统 SHALL 自动检测配置变化并导出到 config.json，无论修改来自屏幕界面、Web 页面还是控制台；导出 MUST 在变化发生后数秒内完成，不要求用户手动触发。

#### Scenario: Web 修改自动落盘
- **WHEN** 用户在 Web 页面打开一路串口
- **THEN** config.json 在数秒内更新为该串口已启用

#### Scenario: 屏幕修改自动落盘
- **WHEN** 用户在屏幕界面修改串口参数或分段时长
- **THEN** config.json 在数秒内反映相同修改

### Requirement: 配置校验
系统 SHALL 在应用配置前逐字段验证（串口波特率、数据位、停止位、校验位、分段时长、WiFi 名称密码、TCP 端口、蓝牙名称等），任一字段无效时 MUST 整体拒绝该配置且不改变当前运行参数。

#### Scenario: 非法波特率被拒绝
- **WHEN** 导入的配置中某串口波特率超出允许范围
- **THEN** 系统返回配置无效、拒绝应用，设备继续按原配置运行

### Requirement: 备份与恢复
系统 SHALL 在每次覆盖 config.json 前将现有文件备份为 config.json.bak，并支持从备份恢复配置。

#### Scenario: 覆盖产生备份
- **WHEN** 配置变化导致 config.json 被重写
- **THEN** 重写前的内容完整保留在 config.json.bak

#### Scenario: 恢复备份
- **WHEN** 用户触发从备份恢复
- **THEN** 系统加载并应用 config.json.bak 中的配置

### Requirement: 配置操作入口
系统 SHALL 在 Web 页面提供配置的下载、立即导出、从卡重载、恢复备份与上传功能，并在控制台提供等价的配置命令。

#### Scenario: 上传配置文件
- **WHEN** 用户在 Web 页面上传一份合法 config.json
- **THEN** 系统校验通过后立即应用并持久化该配置

#### Scenario: 上传非法文件
- **WHEN** 用户上传的内容不是合法或有效的配置
- **THEN** 系统拒绝该内容并提示配置无效，当前配置不受影响
