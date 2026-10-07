# Proposal

## Why

现场部署后发现日志可读性和配置管理两方面的问题：一个完整 Modbus 应答帧会被按 UART 读取块拆成多行、多个时间戳，请求与应答有时又合并在同一行，且日志没有收发方向标识；同时串口参数、WiFi、透传配置分散在三处 NVS，无法通过 TF 卡直接备份、迁移或在电脑端编辑。本次增强让日志准确呈现「一帧一行、方向明确、可解析」，并以 TF 卡上的单一 JSON 文件统一管理全部配置。

## What Changes

- 重写字节组装逻辑：帧缓冲扩展到 520 字节，帧边界按波特率计算的 Modbus 3.5 字符时间判定（ASCII 帧另按 `:` 起始 / CRLF 结束判定），保证一个报文帧始终独占一行，不再被读取块拆断。
- 每条日志记录毫秒级时间戳和方向标识：设备自身发出记为 `TX`、总线监听记为 `RX`，并进一步标注 `REQ`/`RSP` 角色。
- 新增 Modbus 协议解析：自动识别 Modbus RTU 与 Modbus ASCII，校验 RTU CRC16 / ASCII LRC，结构化提取设备地址、功能码、起始地址、数量/字节数、数值，并在日志中追加一行全 ASCII 解析注释；非 Modbus 数据保持原始 HEX 记录。
- 设备所有发送路径（测试发送、周期发送、控制台注入、TCP/BLE 下发）统一走 logger 出站队列，记录为 TX 并丢弃自身回环 echo。
- 新增 TF 卡根目录 `config.json`：统一管理串口参数、日志分段、WiFi、TCP/BLE 透传配置；启动时自动加载，运行期每 2 秒检测配置变化并自动导出（屏幕与 Web 修改均自动落盘）。
- 配置写入前自动备份 `config.json.bak`；导入时逐字段校验，无效整体拒绝；Web 页面与控制台提供导出、重载、恢复备份、上传入口。

## Capabilities

### New Capabilities

- `frame-logging`: 串口字节流按帧组装与落盘，包括收发方向与请求/应答标识、毫秒级时间戳、出站帧的统一记录。
- `modbus-parsing`: Modbus RTU/ASCII 自动识别、帧校验、结构化字段提取与解析注释生成。
- `config-management`: 基于 config.json 的统一配置加载、自动双向同步、校验、备份与恢复。

### Modified Capabilities

（无——项目此前未建立 OpenSpec 能力清单。）

## Impact

- 新增模块：`main/app_modbus.[ch]`、`main/app_config.[ch]`。
- 修改模块：`main/app_logger.[ch]`（帧组装、出站队列、自适应快速轮询、变化检测）、`main/app_bridge.c`（TCP/BLE 下发改走出站队列、新增 `app_bridge_persist`）、`main/app_wifi.[ch]`（新增 `app_wifi_persist`）、`main/app_httpd.c`（新增 `/api/config`、`/api/config_upload` 与配置管理网页卡片）、`main/app_cmd.c`（新增 `cfg` 命令）、`main/main.c`（启动加载）、`main/CMakeLists.txt`。
- 依赖：新增对 ESP-IDF `json`（cJSON）组件的使用；所有大缓冲与 cJSON 分配位于 PSRAM，不改变 BLE/WiFi 初始化顺序约束。
