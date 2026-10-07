# modbus-parsing Specification

## Purpose

对记录到的报文帧自动进行 Modbus 协议识别与结构化解析，输出人工可读的字段说明，帮助现场快速理解读写操作内容与异常。

## Requirements

### Requirement: RTU 与 ASCII 自动识别
系统 SHALL 自动区分 Modbus RTU 与 Modbus ASCII 帧，无需用户预先指定：ASCII 帧以 `:` 起始并使用 LRC，其余按 RTU 处理并使用 CRC16。

#### Scenario: 两种帧混合出现
- **WHEN** 同一串口先后出现 RTU 二进制帧与 `:` 起始的 ASCII 帧
- **THEN** 两帧分别按 RTU、ASCII 规则解析，注释中正确标注 `MB-RTU` 与 `MB-ASCII`

### Requirement: 帧完整性校验
系统 SHALL 对每条 Modbus 帧执行校验：RTU 使用 CRC16（按低字节先传的线序），ASCII 使用 LRC；校验结果 MUST 在解析输出中标明为通过或失败。

#### Scenario: 校验失败
- **WHEN** 帧结构可识别但校验字段不匹配
- **THEN** 系统仍输出该帧的字段解析，并明确标注校验失败

### Requirement: 结构化字段提取
系统 SHALL 从帧中提取设备地址、功能码及与功能码对应的关键字段（如起始地址、寄存器/线圈数量、数据字节数、数值），并识别异常响应及其异常码。

#### Scenario: 读保持寄存器请求
- **WHEN** 帧为地址 1、功能码 3、起始地址 0、数量 10 的请求
- **THEN** 解析输出包含 addr=1、fc=3、start=0、qty=10

#### Scenario: 读保持寄存器应答
- **WHEN** 帧为功能码 3、字节数 20 的应答
- **THEN** 解析输出包含 bytes=20 及正确的请求/应答角色

#### Scenario: 异常响应
- **WHEN** 帧的功能码最高位置位并携带异常码
- **THEN** 解析输出标注为异常响应并给出异常码

### Requirement: 解析注释附加且不破坏原始数据
系统 SHALL 在报文行之后以独立注释行给出解析结果，注释内容 MUST 为 ASCII 字符；无法识别为 Modbus 的帧 MUST 保持原始十六进制记录，不得臆造解析。

#### Scenario: 非 Modbus 数据
- **WHEN** 一条帧不符合任何 Modbus 帧结构
- **THEN** 日志仅记录时间戳、方向与原始十六进制，不追加解析注释
