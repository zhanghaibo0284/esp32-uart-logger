# ESP32-S3 多路串口记录仪（UART Logger）

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)](https://www.espressif.com/en/products/socs/esp32-s3)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.3.1-red)](https://github.com/espressif/esp-idf/tree/v5.3.1)
[![LVGL](https://img.shields.io/badge/LVGL-8.4.0-green)](https://github.com/lvgl/lvgl/tree/release/v8.4)
[![License](https://img.shields.io/badge/license-MIT-brightgreen)](LICENSE)

一款可长期部署在现场、独立抓取多路设备串口数据的记录仪。基于正点原子 ESP32-S3 BOX 开发板，集 **本地触屏操作、TF 卡日志落盘、WiFi Web 管理、TCP / BLE 无线透传** 于一体，无需上位机即可完成串口数据的采集、存储与回取。

---

## 目录

- [功能特性](#功能特性)
- [硬件平台](#硬件平台)
- [技术栈](#技术栈)
- [快速开始](#快速开始)
  - [环境搭建](#1-环境搭建)
  - [依赖安装](#2-依赖安装)
  - [编译](#3-编译)
  - [烧录](#4-烧录)
  - [运行与访问](#5-运行与访问)
- [Web 管理界面与 API](#web-管理界面与-api)
- [项目目录结构](#项目目录结构)
- [配置说明](#配置说明)
- [常见问题排查](#常见问题排查)
- [相关文档](#相关文档)
- [贡献指南](#贡献指南)
- [开源协议](#开源协议)

---

## 功能特性

### 数据采集与存储

- **三路独立串口**（UART0 / UART1-J2 / UART2-J3），波特率、数据位、停止位、校验、开关均可独立配置，参数持久化到 NVS
- **日志自动落盘 TF 卡**：按设定分段时长滚动生成文件，目录结构 `/sdcard/YYYY-MM-DD/UARTx/*.txt`，每行为 `毫秒时间戳: HEX数据`（大写 16 进制）
- **事件驱动接收 + 独立磁盘写入线程**：高速主从流量下不丢帧、不粘连，支持 64 位字节计数（无溢出）
- **Modbus 解析**：帧边界识别与帧日志格式化，规则由 TF 卡上的 `config.json` 配置
- **手动 / 手机时间校时**：报文时间戳可信（复位后可恢复 RTC 时间）

### 本地触屏（LVGL 8.4）

- 六个标签页：状态 / 串口 / 时钟 / 日志 / 文件 / WIFI
- 屏幕端文件浏览器：进入目录、打开查看文件内容，支持大列表（>80 文件）滚动
- 屏幕按键控制与自动息屏省电

### WiFi Web 管理（SoftAP + 内置 HTTP 服务器）

- 默认 AP：`UART-LOG` / `12345678`，地址 `192.168.4.1`，带 captive portal DNS
- 串口参数配置、开关与状态查看
- 手动校时 / 一键「用手机时间」
- **文件管理**：面包屑 + 路径输入跳转、目录下载（任意深度嵌套递归打包为 ZIP，带实时进度）、多选 / 全选下载
- **多路实时数据流**：网页勾选通道实时查看串口数据（增量游标轮询）

### 无线透传

- **WiFi TCP 透传**：每路串口一个 TCP 端口（默认 8081/8082/8083），双向，端口可改
- **蓝牙 BLE UART（NUS）透传**：NimBLE 协议栈，手机 / 电脑用 nRF Connect 连接，双向，与 SoftAP 共存

### 其它

- **USB Serial JTAG 控制台命令行**：状态查询、参数设置、inject 注入、日志测试等
- **USB Serial JTAG 同时作为烧录 / 日志口**（COM 口，不占用业务 UART0）

## 硬件平台

| 项 | 规格 |
|---|---|
| 开发板 | 正点原子 ESP32-S3 BOX |
| 芯片 | ESP32-S3（QFN56，修订 v0.2） |
| Flash | 16 MB |
| PSRAM | 8 MB 八线（Octal SPI） |
| 屏幕 | 电容触摸屏（LVGL 8.4 驱动） |
| TF 卡 | SD-SPI 接口（实测 64 GB，exFAT 亦可挂载） |
| 扩展 IO | 板载 I2C 扩展芯片 xl9555（LED 等） |

### 引脚映射

| 功能 | GPIO | 对外接口 |
|---|---|---|
| UART0 TX / RX | 43 / 44 | 板载 |
| UART1 TX / RX | 5 / 6 | **J2** |
| UART2 TX / RX | 8 / 18 | **J3** |
| TF 卡 MOSI / MISO / SCLK / CS | 16 / 15 / 7 / 17 | SD-SPI |

> 修改引脚只需改 `main/board.h` 中的串口表和 `main/app_sd.c` 中的 SD 卡表。

## 技术栈

| 层 | 技术 |
|---|---|
| 开发框架 | ESP-IDF **v5.3.1**（FreeRTOS） |
| 图形界面 | LVGL **8.4.0**（自定义中文字体，按需收字） |
| 蓝牙 | NimBLE（Nordic UART Service GATT，Bluedroid 关闭） |
| 网络 | esp_http_server + lwIP + SoftAP + captive portal DNS |
| 文件系统 | FatFS（LFN heap、UTF-8），SD-SPI 挂载 TF 卡 |
| Web 前端 | 原生 HTML / JavaScript（无框架，转义嵌入固件） |
| 语言 | C（固件） |
| 存储 | NVS（参数持久化）、TF 卡（日志） |

关键内存策略：开启 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` 让 WiFi / lwIP 优先使用 PSRAM，是 BLE + WiFi + httpd 三者共存的关键。

## 快速开始

### 1. 环境搭建

1. 安装 [ESP-IDF v5.3.1](https://docs.espressif.com/projects/esp-idf/zh_CN/v5.3.1/esp32s3/get-started/index.html)（Windows 推荐安装到 `C:\Espressif`）。
2. 安装 Python 依赖（PC 端辅助脚本用）：

   ```powershell
   pip install pyserial
   ```

3. 克隆仓库（**必须使用纯 ASCII 路径**，中文路径会导致链接脚本生成失败）：

   ```powershell
   git clone https://github.com/zhanghaibo0284/esp32-uart-logger.git F:\esp32_uart_log
   ```

### 2. 依赖安装

固件侧依赖只有一个受管组件 **LVGL 8.4.0**（见 `main/idf_component.yml`），首次编译时由 idf.py 自动拉取到 `managed_components/`，无需手动安装。

### 3. 编译

```bat
cmd /c "F:\esp32_uart_log\build_ascii.bat 2>&1"
```

构建脚本已内置 `IDF_PATH` 与 `PYTHONUTF8=1` 设置。首次编译会依据 `sdkconfig.defaults` 自动生成 `sdkconfig`。

### 4. 烧录

通过 USB 下载口（识别为 COM 口，本项目为 COM3）烧录：

```powershell
cd F:\esp32_uart_log\build
C:\Espressif\python_env\idf5.3_py3.11_env\Scripts\python.exe -m esptool `
  --chip esp32s3 -p COM3 -b 460800 --before default_reset --after hard_reset `
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m @flash_args
```

### 5. 运行与访问

1. **串口监控**（115200）：

   ```powershell
   python mon_serial.py     # 持续读 25 秒
   python read_boot.py      # 复位后读 7 秒（抓启动序列）
   ```

   启动日志应依次出现 `ui ready`、`Bluetooth MAC`、`ble stack ready`、AP 启动信息，**无** `Invalid mbox` / `BLE_INIT` 报错。

2. **触屏操作**：开机即进入六标签页界面。

3. **Web 管理**：手机 / 电脑连接 WiFi `UART-LOG`（密码 `12345678`），浏览器访问 <http://192.168.4.1>。

4. **TCP 透传**：连接 AP 后 `telnet 192.168.4.1 8081`（8082 / 8083 对应另外两路）。

5. **BLE 透传**：用 nRF Connect 搜索 `UART-LOG-BLE`，连接 NUS 服务即可双向收发。

## Web 管理界面与 API

内置网页提供：串口卡片（参数 + 开关）、串口透传卡片（TCP 端口 / BLE 绑定与名称）、实时数据（三通道勾选 + 清空）、校时（手填 + 用手机时间）、日志文件（面包屑 / 路径输入 / 多选 / 全选 / 文件夹打包下载）。

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/` | 返回内嵌网页 |
| GET | `/api/status` | 时间、SD 容量、SSID、三路串口状态与参数 |
| GET | `/api/files?dir=<path>` | 列目录（默认仅直接子项；`rec=1` 强制记录视图） |
| GET | `/api/port?index=&baud=&bits=&stop=&parity=&on=` | 设置某路串口参数并开关 |
| GET | `/api/time?y=&mo=&d=&h=&mi=&s=` | 校时 |
| GET | `/dl?path=<file>` | 下载单个文件 |
| GET | `/api/zip?dir=<dir>&names=a\|b\|c` 或 `&all=1` | 打包下载选中 / 整目录（任意深度递归） |
| GET | `/api/live?<i>=<cursor>...` | 多路实时数据流（增量，带 next 游标） |
| GET | `/api/bridge` | TCP / BLE 透传状态 |
| GET | `/api/bridge_tcp?index=&on=&port=` | 开关某路 TCP 透传及端口 |
| GET | `/api/bridge_ble?index=&on=&name=<urlenc>` | 开关 BLE 广播、绑定串口、改名 |

## 项目目录结构

```
esp32-uart-logger/
├─ CMakeLists.txt            # 顶层工程，project(uart_logger)
├─ partitions.csv            # 自定义分区表（4MB app）
├─ sdkconfig.defaults        # 基线配置（PSRAM/Flash/LVGL/FATFS 等）
├─ build_ascii.bat           # ★ 编译入口
├─ mon_serial.py             # 持续读串口 25 秒
├─ read_boot.py              # 拉 RTS 复位后读 7 秒（抓启动序列）
├─ probe_tcp.py / cap_ble*.py / set_clock.py   # 诊断与校时脚本
├─ AGENTS.md                 # 开发规格说明书（踩坑记录、硬约束，改代码前必读）
├─ CHANGELOG.md              # 环境与变更全记录（换设备恢复环境用）
├─ openspec/                 # OpenSpec 规格（5 份主 spec + 归档 change）
├─ components/
│  └─ BSP/                   # 正点原子板级支持（myiic / xl9555 / led / LCD）
├─ managed_components/       # LVGL 8.4.0（idf.py 自动拉取，不入库）
└─ main/
   ├─ main.c                 # app_main：唯一初始化顺序控制点
   ├─ board.h                # 串口数量与 TX/RX 引脚
   ├─ app_settings.[ch]      # 全局配置（分段时长 + 各串口参数），NVS
   ├─ app_time.[ch]          # RTC 时间 set / restore / persist
   ├─ app_sd.[ch]            # TF 卡 SD-SPI 挂载 / 容量 / 日期目录 / 文件锁
   ├─ app_logger.[ch]        # ★ 核心：串口收发、日志落盘、周期发送、实时缓冲
   ├─ app_modbus.[ch]        # Modbus 帧解析与格式化
   ├─ app_config.[ch]        # config.json 配置管理
   ├─ app_ui.[ch]            # LVGL 六标签页
   ├─ app_files.[ch]         # 屏幕端文件浏览 / 查看
   ├─ app_screen.[ch]        # 屏幕按键与自动息屏
   ├─ lvgl_port.[ch]         # LVGL 显示 / 触摸移植
   ├─ font_cn_16.[ch]        # 自定义中文字体（仅含 symbols.txt 内汉字）
   ├─ symbols.txt            # 中文字库汉字清单
   ├─ app_wifi.[ch]          # SoftAP 启停 / SSID / 密码 NVS
   ├─ app_httpd.[ch]         # HTTP 服务器 + 内嵌网页 + 全部 API
   ├─ web_index.html         # 网页源文件（改后须重新嵌入 WEB_PAGE）
   ├─ app_bridge.[ch]        # WiFi TCP + BLE NUS 透传
   ├─ dns_server.[ch]        # captive portal DNS
   └─ app_cmd.[ch]           # USB Serial JTAG 控制台命令
```

## 配置说明

| 配置项 | 位置 | 说明 |
|---|---|---|
| 分区表 | `partitions.csv` | nvs 0x6000 / phy_init 0x1000 / factory 4MB |
| 基线系统配置 | `sdkconfig.defaults` | Flash 16MB、240MHz、8MB 八线 PSRAM、FATFS UTF-8、LVGL dark theme |
| 蓝牙 | `sdkconfig` | `CONFIG_BT_ENABLED=y`、`CONFIG_BT_NIMBLE_ENABLED=y`、单连接 |
| 串口参数 / 分段时长 | NVS namespace `logger` | 屏幕或 Web 修改后自动持久化 |
| AP SSID / 密码 | NVS `logger`（`ap_ssid`/`ap_pass`） | 默认 `UART-LOG` / `12345678` |
| 透传配置 | NVS namespace `bridge` | TCP 端口、BLE 绑定与设备名 |
| Modbus 规则 | TF 卡 `config.json` | 帧解析规则，可在线编辑 |

## 常见问题排查

| 现象 | 原因与对策 |
|---|---|
| 反复重启 / 白屏黑屏切换 | 用 `read_boot.py` 抓启动日志。两类典型：①`assert failed: tcpip_send_msg_wait_sem ... Invalid mbox` —— socket 用得太早，网络栈未起；②`BLE_INIT: Malloc failed` —— BLE 初始化放在了 WiFi 之后。初始化顺序见 [AGENTS.md](AGENTS.md) 第 6 节 |
| 屏幕不亮但设备未重启 | UI 任务创建失败（内部 RAM 不足）。串口查 `ui ready` / `ui task create failed`；UI 任务须在 BLE / WiFi 之前创建 |
| Web 接口全部超时但 AP 正常 | ①确认电脑连的是记录仪 AP：`ping 192.168.4.1` 的 **TTL 必须为 64**（TTL=251 是本机虚拟网卡 / VPN 在应答）；②不要在 httpd 工作线程里做重初始化 |
| `httpd start failed` | 内部 RAM 耗尽。确认 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，LCD i80 的 `max_transfer_bytes` 按刷新块（约 25KB）而非整屏设置 |
| BLE 广播搜不到 / `adv set fields failed: 4` | 广播数据超 31 字节上限：只放 flags + 完整设备名，**不要把 128 位 UUID 放进广播包** |
| 屏幕汉字显示成小方块 | 中文字体只包含 `main/symbols.txt` 内汉字；新汉字须先加进 symbols.txt 再重新生成 `font_cn_16.c` |
| 改了网页但没生效 | `web_index.html` 改动后必须重新转义并同步进 `app_httpd.c` 顶部的 `WEB_PAGE` 内嵌副本 |
| 中文路径编译失败 | ldgen / objdump 编码损坏。工程必须放在纯 ASCII 路径（如 `F:\esp32_uart_log`） |
| 串口监控一打开设备就死机 | pyserial 打开 COM 口会拉 RTS 把设备按在复位态；用 .NET `SerialPort` 并在 `Open()` 前设 `DtrEnable=$false; RtsEnable=$false` |

## 相关文档

- [AGENTS.md](AGENTS.md) — 开发规格说明书：初始化顺序、关键路径警告、修改网页 / 字体的硬约束（**改代码前必读**）
- [CHANGELOG.md](CHANGELOG.md) — 环境与变更全记录：工具链版本、配置明细、提交历史、待办清单、新环境恢复步骤
- [openspec/](openspec/) — OpenSpec 功能规格（config-management / frame-logging / modbus-parsing / screen-power / traffic-statistics）

## 贡献指南

欢迎提交 Issue 和 Pull Request。提交代码前请遵守以下约定（详见 [AGENTS.md](AGENTS.md) 第 15 节）：

1. **初始化顺序不可随意调整**——BLE 必须在 WiFi 之前初始化，TCP 透传必须在网络栈就绪之后恢复，UI 任务须尽早创建并检查返回值。
2. 优先复用 ESP-IDF 官方示例与成熟开源组件，改动保持最小、风格与现有代码一致。
3. 配置一律持久化到 NVS；跨任务操作通过锁（`s_lock` / `app_fs_lock`）与异步请求标志，不在他任务上下文直接操作硬件 / 文件系统。
4. 改 NVS blob 结构体记得同步递增 magic；改 `web_index.html` 记得重新嵌入 `WEB_PAGE`；加屏幕汉字先进 `symbols.txt`。
5. 不在代码里添加版权 / 协议头；工程必须保持在纯 ASCII 路径下编译。
6. 验证路径：先针对性跑相关功能，再整体烧录实测，串口日志是主要判据。

## 开源协议

本项目基于 [MIT License](LICENSE) 开源。

---

> 如果这个项目对你有帮助，欢迎 Star ⭐
