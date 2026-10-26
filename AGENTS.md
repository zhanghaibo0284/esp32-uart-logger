# AGENTS.md — ESP32-S3 串口记录仪 开发规格说明书

> 本文件供新会话 / 其他智能体快速接手本项目。**动手前务必完整读完本文件**，尤其是「关键路径警告」和「初始化顺序」两节，这两条踩中会直接导致编译失败或设备反复重启。

---

## 1. 项目概述

长时间部署在现场、抓取多路设备串口数据的独立记录仪。基于正点原子 ESP32-S3 BOX 开发板，支持：

- LVGL 触摸屏菜单（状态 / 串口 / 时钟 / 日志 / 文件 / WIFI 六个标签页）
- 三路串口独立配置（波特率、数据位、停止位、校验、开关、状态指示）
- 手动 / 手机时间校时（报文需带时间戳）
- 日志按设定分段时长滚动，写入 TF 卡；每个串口一个文件夹，毫秒时间戳 + 16 进制
- 屏幕端文件浏览器（进入目录、打开查看内容）
- WiFi SoftAP + 内置 Web 服务器：参数配置、手机校时、文件浏览/下载/多选/全选 ZIP、多路实时数据流
- WiFi TCP 透传（每路串口一个 TCP 端口，双向）
- 蓝牙 BLE UART(NUS) 透传（手机/电脑用 nRF Connect 等连接，双向）
- USB Serial JTAG 控制台命令行

**规划中**：无屏幕版本，全部配置依赖 Web（配置已全部走 NVS、Web API 已较完整，是良好基础）。

---

## 2. 硬件平台

- 板子：正点原子 ESP32-S3 BOX
- 模组/芯片：ESP32-S3（QFN56，芯片修订 v0.2）
- Flash：16 MB（实际烧录 qio/dio，见构建命令）
- PSRAM：8 MB 八线（octal SPI，AP_3v3）
- 屏幕：电容触摸屏，LVGL 8.4
- TF 卡：通过 SD-SPI 接口（见引脚），现用 64 GB（exFAT 亦可挂载）
- 下载口：USB，**端口 COM3**（esptool 识别 VID_303A&PID_1001），app 偏移 `0x10000`
- 工具链：ESP-IDF **v5.3.1**，安装在 `C:\Espressif\frameworks\esp-idf-v5.3.1`
- 板载 I2C 扩展 IO：`myiic` + `xl9555`（BSP 组件提供），LED 由扩展 IO 控制

---

## 3. 关键路径警告（最重要）

- **可工作工程位于纯 ASCII 路径：`F:\esp32_uart_log`。所有编辑、编译、烧录都在这里进行。**
- 原始工程在中文路径 `F:\B\硬件设计\esp32串口记录仪`，**仅作资料/备份，绝不在该路径下编译**。中文路径会导致 ldgen / objdump 编码损坏。
- 正点原子资料盘（只读参考）：`E:\B\RDWorks\Embed\esp\正点原子\【正点原子】ESP32S3 BOX开发板\【正点原子】ESP32S3 BOX开发板资料（A盘）`
- 官方蓝牙示例（BLE 代码以此为准，优先复用不要自己造轮子）：
  `C:\Espressif\frameworks\esp-idf-v5.3.1\examples\bluetooth\nimble\bleprph\main\main.c`

**Shell 注意事项**：
- PowerShell 内联命令里的 `&`（URL 查询串）、`$_`、`$变量` 会被错误解析。
- 处理含 `?a=1&b=2` 的 URL：用 `cmd /c curl.exe "<url>"`（加引号）或用 Node `fetch`。
- 编辑文件优先用 `apply_patch`；若 PowerShell here-string 把补丁解析坏，改用 Node `fs` 精确读写（本项目已多次这样处理）。

---

## 4. 目录结构

```
F:\esp32_uart_log
├─ CMakeLists.txt            # 顶层工程，project(uart_logger)
├─ partitions.csv            # 自定义分区表
├─ sdkconfig                 # 当前生效配置
├─ sdkconfig.defaults        # 基线配置（PSRAM/Flash/LVGL/FATFS 等）
├─ build_ascii.bat           # ★编译入口
├─ build_bridge.bat / build2_all.bat  # 其它历史构建脚本
├─ mon_serial.py             # 持续读 COM3 25 秒
├─ read_boot.py              # 拉 RTS 复位后读 7 秒
├─ cap_ble.py / cap_ble_long.py       # 复位+抓日志+中途触发 BLE 的诊断脚本
├─ probe_tcp.py              # 复位后每秒探测 80/53 端口
├─ uart_log_profile.xml      # Windows WiFi 配置（SSID UART-LOG）
├─ components/
│  └─ BSP/                   # 正点原子板级支持（myiic/xl9555/led/LCD 等）
├─ managed_components/       # LVGL 等受管组件（idf_component.yml）
└─ main/
   ├─ main.c                 # app_main：唯一初始化顺序控制点
   ├─ board.h                # 串口数量与 TX/RX 引脚
   ├─ app_settings.[ch]      # 全局配置（分段时长 + 各串口参数），NVS
   ├─ app_time.[ch]          # RTC 时间，set/restore/persist
   ├─ app_sd.[ch]            # TF 卡 SD-SPI 挂载 / 容量 / 文件锁
   ├─ app_logger.[ch]        # ★核心：串口收发、日志落盘、周期发送、实时缓冲
   ├─ app_files.[ch]         # LVGL 文件浏览/查看界面
   ├─ app_ui.[ch]            # LVGL 六个标签页
   ├─ lvgl_port.[ch]         # LVGL 显示/输入移植初始化
   ├─ font_cn_16.[ch]        # 自定义中文字体（仅含 symbols.txt 内汉字）
   ├─ symbols.txt            # ★中文字库所含汉字清单
   ├─ app_wifi.[ch]          # SoftAP 启停 / SSID/密码 NVS
   ├─ app_httpd.[ch]         # HTTP 服务器 + 内嵌网页 + 全部 API
   ├─ web_index.html         # 网页源文件（改动后需重新嵌入，见第 11 节）
   ├─ app_bridge.[ch]        # WiFi TCP + BLE NUS 透传
   ├─ dns_server.[ch]        # captive portal DNS（UDP 53 重定向）
   ├─ app_cmd.[ch]           # USB Serial JTAG 控制台命令
   ├─ idf_component.yml      # 依赖：lvgl 8.4
   └─ CMakeLists.txt         # 组件注册（REQUIRES 含 bt 等）
```

---

## 5. 引脚与串口映射

串口索引即 UART 编号（`uart_port_t`），定义在 `main/board.h`：

| 索引 | 端口 | TX | RX | 对外面 | 说明 |
|------|------|----|----|--------|------|
| 0 | UART0 | 43 | 44 | 板载 | 非下载控制台用途的一路串口 |
| 1 | UART1 | 5  | 6  | **J2** | |
| 2 | UART2 | 8  | 18 | **J3** | |

`APP_PORT_COUNT = 3`。修改引脚只改 `board.h` 中两张表。

TF 卡（SD-SPI，`main/app_sd.c`）：

| 信号 | GPIO |
|------|------|
| MOSI | 16 |
| MISO | 15 |
| SCLK | 7  |
| CS   | 17 |

---

## 6. 初始化顺序（强约束，曾导致反复重启）

当前 `main/app_main()` 的顺序（`main/main.c`），**不要随意调整**：

```
1. nvs_flash_init()
2. app_time_init() / app_settings_init() / app_time_restore()
3. led_init() / iic_init() / xl9555_init()
4. app_sd_mount() / app_config_init() / app_config_load()
5. app_ota_start()          // 读取运行槽/备用槽、PENDING_VERIFY 状态
6. app_logger_start()
7. app_mbsim_start()        // ★必须在 logger 之后：主站规则用 app_logger_request_tx() 出队
8. 创建 ui_task（xTaskCreatePinnedToCore, core=1）★须在 BLE/WiFi 之前，并检查返回值
9. app_bridge_ble_init()    // ★BLE 控制器 + NimBLE 主机，必须在 WiFi 之前
10. app_cmd_start()
11. (可选) app_logger_request_sd_test()
12. app_wifi_start()        // esp_netif_init / 默认事件循环 / SoftAP
13. 若 wifi.up → app_httpd_start()
14. app_bridge_tcp_resume() // ★TCP 透传，必须在网络栈就绪之后
```

**为什么是这个顺序（两个真实踩过的坑）**：

- BLE 控制器 `nimble_port_init()` 需要一块较大的**内部连续 DMA 内存**。若先启动 WiFi/lwIP，内部内存被占用，控制器初始化报 `ESP_ERR_NO_MEM`（串口可见 `BLE_INIT: Malloc failed / controller init failed`）。所以 BLE 必须在 WiFi **之前**初始化。NimBLE 主机本身不依赖 lwIP。
- TCP 透传要调用 BSD socket，依赖 lwIP 的 tcpip 线程/mbox（在 `esp_netif_init` / WiFi 初始化时才创建）。在网络栈就绪前调 `socket()` 会触发
  `assert failed: tcpip_send_msg_wait_sem ... tcpip.c:449 (Invalid mbox)` 并重启。所以 TCP 监听必须在 WiFi/httpd **之后**恢复。
- 不要在 httpd 工作线程里执行 `nimble_port_init()` 这类重初始化，会长时间/永久阻塞单线程的 `esp_http_server`，表现为所有接口超时。Web 端「开启BLE」只切换广播（`ble_gap_adv_start/stop`），协议栈开机时已建好。
- **UI 任务也要尽早创建并检查返回值**：任务栈 16KB 占内部内存，若放到 BLE/WiFi 之后创建，内部 RAM 不足会 `xTaskCreatePinnedToCore` 静默失败（返回值非 pdPASS），表现为屏幕完全不亮、背光不亮，但设备并未重启（串口监控无输出）。现放在 app_logger_start 之后、BLE 之前，并判断返回值、失败打印 `ui task create failed`。
- **BLE 广播数据有 31 字节硬上限**：flags(3) + 完整设备名 + 128 位 Service UUID(16) 往往超 31，`ble_gap_adv_set_fields` 返回错误码 4（`adv set fields failed: 4`）。解决：广播包只放 flags + 完整名称，**不要把 128 位 UUID 放进广播数据**（UUID 仍在 GATT 服务里，连接后照样可枚举；需要可放进 scan response）。
- **httpd 起不来（`httpd start failed`）多半是内部 RAM 耗尽**：BLE+WiFi 先后占满内部内存后，HTTP 服务器建任务/socket 失败。PSRAM 机型标准解法是 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`（让 WiFi/lwIP 在安全前提下尽量用 PSRAM，腾出内部 RAM）。
- **LCD i80 总线不要按整屏预留内部 DMA**：`bus_config.max_transfer_bytes` 若写成 `宽*高*2 = 320*240*2 ≈ 150KB` 会长期占掉大块内部 RAM。实际只按刷新块大小设置（现 `宽*40*2 = 25600 字节`），配合 LVGL 分块刷新即可。

---

## 7. 构建 / 烧录 / 监控

**编译**（脚本内已 set IDF_PATH、PYTHONUTF8=1 并 cd 到 ASCII 目录）：

```
cmd /c "F:\esp32_uart_log\build_ascii.bat 2>&1"
```

**烧录**（在 build 目录，用 @flash_args；flash dio / 80m / 16MB）：

```
cd /d F:\esp32_uart_log\build
C:\Espressif\python_env\idf5.3_py3.11_env\Scripts\python.exe -m esptool ^
  --chip esp32s3 -p COM3 -b 460800 --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m @flash_args
```

`flash_args` 内含：`0x0 bootloader.bin`、`0x8000 partition-table.bin`、`0xf000 ota_data_initial.bin`、`0x20000 uart_logger.bin`。

> 分区表改为 OTA 三槽后 app 偏移是 **0x20000**（旧工程为 0x10000）。从旧固件升级到本版本时必须整片烧录（含 `ota_data_initial.bin`），否则 bootloader 找不到 factory 镜像。`phy_init` 已从 0xf000 挪到 0x11000，原位置被 `otadata` 占用。

**串口监控**（115200）：

- `cmd /c "cd /d F:\esp32_uart_log && python mon_serial.py"` — 持续读 25 秒
- `... python read_boot.py"` — 拉 RTS 复位后读 7 秒（抓启动序列）

**重要环境排错经验**：测 Web 前先确认电脑真的连在记录仪 AP（`UART-LOG`）上，而不是家里路由器。曾出现电脑连家里路由器时 `ping 192.168.4.1` 仍通但 **TTL=251**（正常 ESP32 lwIP 为 64），那是本机虚拟网卡/VPN 在应答，流量没到设备，表现为网页「failure」、TCP 80 超时。判据：TTL 是否为 64、`netsh wlan show interfaces` 的 SSID。

---

## 8. 功能模块说明

### app_settings
- 结构：`segment_min`（日志分段，分钟）+ `disk_limit_pct`（存储清理阈值，1–99，默认 90）+ 每路 `port_setting_t`（baud / data_bits / stop_bits / parity / enabled / `parse_proto`）。
- `parse_proto` 取 `app_parse_proto_t`：`off / auto / modbus_rtu / modbus_ascii / dlt645 / cjt188 / ascii`；`off` 表示只记原始 HEX。
- NVS blob 版本 `CFG_VERSION=4`（v2–v4 均可读，旧 blob 的 `parse_proto` 默认为 off、`disk_limit_pct` 默认 90）。
- 存 NVS；`app_settings_save` 后通常配合 `app_logger_request_reload()` 让串口重新打开生效。

### app_time
- `app_time_set(y,mo,d,h,mi,s)` 设置并标记 trusted；`app_time_restore()` 复位后恢复；`app_time_persist_now()` 落盘。
- `app_time_is_trusted()` 用于判断时间戳是否可信。

### app_sd
- SD-SPI 挂载点 `/sdcard`；提供容量、挂载状态、最后错误。
- `app_fs_lock/unlock`：文件系统互斥（Web 下载与日志写入/屏幕浏览之间）。

### app_logger（核心）
- 每路串口一个任务：`uart_driver_install` + 读循环；收到数据后格式化落盘。
- 日志路径：`/sdcard/UART<x>/YYYYmmdd_HHMMSS.txt`（每路独立文件夹，按分段时长滚动新文件）。
- 每行格式：`时间戳: <HEX...>`，**毫秒级时间戳**，数据体一律大写 16 进制（如 `55 AA 01`）。启用了日志解析的通道会在同一行尾部追加解析注记：
  `2026-10-26 10:00:00.123 RX 01 03 00 00 00 02 C4 0B ; # MB-RTU REQ addr=1 fc=03 ...`。
  `RX / RX-REQ / RX-RSP / TX / TX-REQ` 标记帧方向与角色；`; #` 之后才是解析内容，`off` 通道没有这一段。注释格式见文件首行说明。
- `app_view_t` 汇总给 UI / 命令行：每路 open/error/rx_bytes/drop_lines/tx_frames/tx_bytes/sending/param/file_name，以及 SD 状态和最近一帧 HEX、`cleaned_files`（累计清理文件数）。
- 周期发送：`app_logger_set_periodic_tx(port, interval_ms)` / 停止；单次测试 `request_tx_test`（发 `55 AA 01 02 03`）。
- 注入：`app_logger_inject(port, data, len)`（控制台 inject 用）。
- 实时流：`app_logger_live_get(index, since, ..., &next)` 维护每路环形缓冲与游标，供 `/api/live` 轮询（支持多路同时）。
- 请求类接口（reload/remount/sd_test/clear_logs）通过标志位异步在 logger 上下文执行，避免跨任务直接操作。

#### 日志存储阈值与按通道清理
- 阈值来自 `disk_limit_pct`（默认 90%）。占用 ≥ 阈值时触发，只删**本通道目录**里文件名最早的 `.txt`，**绝不跨通道删除**；轮询各通道每次各删 1 个，直到占用回落到 `阈值-5%`（滞后，避免反复触发）。
- 检测周期：超限时 5s、正常时 30s；`statvfs` 与删除都在 `ports_idle()`（无半帧在组装）时执行，并先 `writer_barrier(2)` 关闭全部文件，避免删正在写的文件。
- 手动清理：`app_logger_request_clear_logs(port)`（`port=-1` 表示每通道各清 200 个），Web 为 `/api/logs_clear?port=`。
- 每次清理后 `app_sd_refresh_usage()` 并累计到 `s_view.cleaned_files`。

### app_parse（串口日志协议解析）
- `app_parse_frame(proto, port, data, len, own_tx, &kind, note, note_len)`：按通道配置解析一帧，返回是否识别；`kind` 为 `AP_KIND_REQ/RSP/DATA`。
- 规则库：Modbus RTU（复用 `app_modbus_frame` 的 CRC 文法，含 fc/寄存器解读）、Modbus ASCII（`:` + LRC）、DL/T 645-2007（0x68…0x16、地址 BCD、DI 表、数值缩放）、CJ/T 188（水/气/热表）、可打印 ASCII（AT 指令等）。
- `auto` 依次尝试 Modbus → DL/T645 → CJ/T188 → ASCII，并要求校验通过；显式协议即使校验失败也返回注记（`check=BAD`），便于现场排错。
- 注记文本很短（<192 字节）且不含换行，直接拼在日志行尾；不改变原始 HEX 记录，`off` 通道完全不额外开销。

### app_ota（固件升级）
- 三槽分区：`factory` / `ota_0` / `ota_1` + `otadata`；运行槽与备用槽由 `esp_ota_get_running_partition()` / `esp_ota_get_next_update_partition()` 决定。
- 流程：Web `POST /api/ota_upload` 边收边写 `/sdcard/ota.bin`（128KB–4MB，校验 `0xE9` 魔数）→ `app_ota_request_apply()` → OTA 任务 `esp_ota_begin/write/end` 写备用槽 → `esp_ota_set_boot_partition` → 删除暂存 → 重启。
- **断电安全**：镜像完整写入并 `esp_ota_end()` 校验通过后才切换 boot 分区；写 flash 期间断电，原槽仍可启动。
- **回滚**：`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`，新镜像以 `PENDING_VERIFY` 启动，`app_ota_tick()` 在 30s 健康窗口后自动 `esp_ota_mark_app_valid_cancel_rollback()`；窗口内失败或主动 `/api/ota_action?do=rollback` 走 `esp_ota_mark_app_invalid_rollback_and_reboot()` 退回旧槽。
- 状态结构 `app_ota_info_t`：available / pending_verify / 槽号 / 版本（`PROJECT_VER`）/ total / written / phase / msg。

### app_modbus_codec + app_mbsim（Modbus 模拟）
- `app_modbus_codec`：CRC16、从站应答（fc 01/02/03/04/05/06/0F/10，异常码 01/02/03，广播写不回应）、主站请求组帧、应答解析。
- `app_mbsim`：复用 logger 的 UART 与出站队列（`app_logger_request_tx`），不重复 open 端口。
  - 从站：地址 1–247，四类寄存器各自独立 `起始地址 + 数量`（各 ≤256），值表常驻内存（不落盘）。
  - 主站：最多 8 条规则，每规则含通道 / 从站 / 功能码 / 起始 / 数量 / 值 / 周期 / 次数；10ms tick，应答超时 600ms，单次执行或周期重复。
  - 同一通道同时配主站规则与从站会被拒绝（会互答）；RS-485 自回显按 60s 窗口前缀匹配跳过。
  - 配置存 NVS namespace `mbsim`、键 `cfg`，magic `0x4D425331`、version 1。


### app_ui / app_files / lvgl_port / font_cn_16
- `lvgl_port_init()` 初始化显示与触摸输入；UI 任务固定在 **core 1**（`main.c` 末尾），栈 16 KB。
- 标签页：`状态 / 串口 / 时钟 / 日志 / 文件 / WIFI`（见 `app_ui.c`，勿在标签标题加新汉字，理由见字体约束）。
- `app_files`：屏幕端文件浏览器，可进目录、选中文件打开查看内容。
- 关于「PSRAM 画屏幕」：LVGL 显示缓冲/对象可分配在 PSRAM，主控把整帧先绘制到 PSRAM 中的 draw buffer，再由刷新回调经 DMA 批量搬到 LCD；PSRAM 容量大但带宽/延迟高于内部 SRAM，故通常用双缓冲/分块刷新。

### app_wifi
- SoftAP，默认 SSID `UART-LOG`、密码 `12345678`、IP `192.168.4.1`，信道 6，最多 4 站。
- SSID/密码存 NVS（namespace `logger`，键 `ap_ssid`/`ap_pass`）；`app_wifi_apply` 校验为可见 ASCII（SSID 1–32，密码 8–63）后停 AP 重启。
- 配套 `dns_server` 实现 captive portal（把所有域名解析到 192.168.4.1）。

---

## 9. Web API 参考（`app_httpd.c`）

服务器配置：`max_open_sockets=4`、`lru_purge_enable=true`、`stack_size=8192`、`max_uri_handlers=24`、`recv_wait_timeout=10`、`send_wait_timeout=20`。

| 方法 | 路径 | 作用 |
|------|------|------|
| GET | `/` | 返回内嵌网页 |
| GET | `/api/status` | 时间、SD/剩余、SSID、三路串口状态与参数（含 `parse`） |
| GET | `/api/files?dir=<path>` | 列目录（区分文件/子目录，供浏览与下载） |
| GET | `/api/port?index=&baud=&bits=&stop=&parity=&on=&parse=` | 设置某路参数、解析协议并开关（`parse` 可选） |
| GET | `/api/time?y=&mo=&d=&h=&mi=&s=` | 手动校时，返回 `{ok,time}` |
| GET | `/dl?path=<file>` | 下载单个文件 |
| GET | `/api/zip?dir=<dir>&names=a|b|c` 或 `&all=1` | 打包下载选中 / 全选 |
| GET | `/api/live?<i>=<cursor>...` | 多路实时数据流（增量，带 next 游标） |
| GET | `/api/bridge` | TCP/BLE 透传状态 |
| GET | `/api/bridge_tcp?index=&on=&port=` | 开关某路 TCP 透传及端口 |
| GET | `/api/bridge_ble?index=&on=&name=<urlenc>` | 开关 BLE 广播、绑定串口、改名 |
| GET | `/api/storage?limit=` | 读/设存储清理阈值，返回 total/free/cleaned |
| GET | `/api/logs_clear?port=` | 清理最早的日志（`port` 省略=每通道各清 200 个） |
| GET | `/api/ota` | OTA 状态（槽位/版本/阶段/暂存大小/待确认） |
| POST | `/api/ota_upload` | 上传固件（原始 bin，按 Content-Length 流式写入 TF 卡暂存） |
| GET | `/api/ota_action?do=apply\|rollback\|confirm` | 安装暂存固件并重启 / 回滚 / 手动确认 |
| GET | `/api/mbsim` | 从站配置 + 全部主站规则 + 统计 |
| GET | `/api/mbsim_cfg?action=slave\|rule\|del\|run` | 配置从站、增改/删除规则、执行规则 |
| GET | `/api/mbsim_reg?type=&addr=&value=` | 读写模拟寄存器/线圈（无 `value` 为读） |

网页主要交互：串口卡片（参数、解析协议、开关）、串口透传卡片（TCP 端口 / BLE 绑定与名称）、实时数据（U0/J2/J3 勾选 + 清空）、校时（手填 + 「用手机时间」）、日志文件（面包屑进目录、多选、全选、下载选中/全部）、日志存储（阈值 + 按通道清理）、固件升级（上传/安装/确认/回滚）、Modbus 模拟（从站参数、规则表、寄存器值）。

`config.json`（TF 卡根目录，每次覆盖前自动留 `config.json.bak`）版本 2：在 v1 基础上新增 `disk_limit_pct` 与每路 `"parse": "<协议名>"`；导入时两者都可缺省（阈值按 90、解析保持当前），协议名非法会整份拒绝。


---

## 10. WiFi TCP / BLE 透传（`app_bridge.c`）

配置存 NVS namespace `bridge`，blob 带 magic（当前 `0x42524432`，**改过结构体字段要递增 magic**，否则旧布局会被误读）。

- **TCP**：每路一个监听 socket，默认端口 U0=8081、J2=8082、J3=8083（可在网页改，需 ≥1024）。单客户端/每路，`bridge_task` 周期 `accept/recv`，收到即 `uart_write_bytes`；串口数据经 `app_bridge_feed_uart` `send` 给客户端。收发计数进状态。
- **BLE（NimBLE，NUS GATT）**：默认设备名 `UART-LOG-BLE`。
  - Service UUID `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - RX（写）`...0002...`：`WRITE | WRITE_NO_RSP`，写入内容透传到绑定串口
  - TX（通知）`...0003...`：`NOTIFY`，绑定串口的数据从此 notify 上报
  - 单连接，MTU 128，按 MTU-3 分包；连接/断开/广播完成事件按 `s_adv_wanted` 自动重启广播。
  - NimBLE 内存已调小：单连接、单 bond、ACL/msys 计数精简（见 sdkconfig 中 `CONFIG_BT_NIMBLE_*`）。
- 数据流入口：logger 收到串口数据时调用 `app_bridge_feed_uart(idx,data,len)`，内部同时分发给 TCP 客户端与 BLE（仅绑定的那路）。

**实测通过状态**：BLE 与 SoftAP 共存正常（启动日志可见 `Bluetooth MAC`、`ble stack ready`、随后 AP 起来），`/api/bridge_ble?on=1` 返回 `{"ok":1}`，广播可被 nRF Connect 搜到。

---

## 11. 修改网页 / 屏幕字体的硬约束

### 网页（`main/web_index.html` → `app_httpd.c`）
- `app_httpd.c` 顶部 `WEB_PAGE` 是网页的**转义嵌入副本**。改 `web_index.html` 后**必须重新转义并同步进 `WEB_PAGE`**，否则线上仍是旧页面。
- 已有可靠做法：用 Node 把 html 转义（处理 `"`、`\`、换行等），再扫描定位 `WEB_PAGE` 首个未转义闭合引号进行替换；该方法已验证可用。

### 中文字体（`font_cn_16.c` / `symbols.txt`）
- 中文字体**只包含 `main/symbols.txt` 里列出的汉字**。若在屏幕 UI 用了清单外的汉字，该字会显示成小方块（这是早期「界面字体都是小方块」的原因）。
- 需要新汉字时：把汉字加入 `symbols.txt`，用字体生成工具重新生成 `font_cn_16.c`（仅按需收字以控制体积）。**不要凭空在 UI 字符串里加新汉字。**

---

## 12. 分区与关键配置

`partitions.csv`（自定义表）：

```
nvs       data  nvs      0x9000  0x6000
otadata   data  ota      0xf000  0x2000
phy_init  data  phy      0x11000 0x1000
factory   app   factory  0x20000 0x400000   # 4MB app（整片烧录的初始镜像）
ota_0     app   ota_0    0x420000 0x400000  # OTA 槽 1
ota_1     app   ota_1    0x820000 0x400000  # OTA 槽 2
```

`sdkconfig.defaults` 要点：目标 esp32s3；Flash 16MB；240MHz；8MB 八线 PSRAM（`SPIRAM_MALLOC_RESERVE_INTERNAL=32768`，给 DMA/内部分配预留；**`SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，WiFi/lwIP 优先用 PSRAM 以省内部 RAM，是 BLE+WiFi+httpd 共存的关键**）；FATFS LFN heap、UTF-8；LVGL（8.4，UTF-8，tabview/dropdown/btn/label/switch/spinbox/flex/dark theme）；主任务栈 8192。OTA 还要求 `CONFIG_PARTITION_TABLE_CUSTOM=y` + `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`（新镜像 PENDING_VERIFY，健康窗口后自动确认，否则回退）。

版本号：顶层 `CMakeLists.txt` 的 `set(PROJECT_VER "2.0.0")` 决定 `/api/ota` 显示的版本，发新固件前记得递增。

蓝牙在 sdkconfig 中：`CONFIG_BT_ENABLED=y`、`CONFIG_BT_NIMBLE_ENABLED=y`（Bluedroid 关闭）。注意：用 findstr 在 sdkconfig 里直接搜可能因编码匹配不到，需要时用 Node/其它方式检索。

`main/CMakeLists.txt` 的 `REQUIRES` 含：`BSP nvs_flash fatfs sdmmc esp_timer driver console esp_wifi esp_event esp_netif esp_http_server bt json app_update`。加新源文件要同时加进 SRCS 列表。编译选项：`-finput-charset=UTF-8 -fexec-charset=UTF-8`。

---

## 13. 已知坑 / 排错速查

- **白屏黑屏反复切换 / 反复重启**：先抓 `read_boot.py`。常见两类：①`Invalid mbox`（socket 用得太早，网络栈未起）；②BLE NO_MEM（BLE 初始化放在了 WiFi 之后）。对照第 6 节顺序。
- **网页点按钮无反应 / 接口全超时但设备没重启**：①确认电脑连的是记录仪 AP（看 SSID、ping TTL=64）；②确认没有在 httpd 线程做重初始化把单线程服务器堵死。
- **某串口「打不开 / 一直打开中」或多串口状态互相覆盖**：历史上出现过多路串口任务/状态共享问题，排查每路任务是否独立、状态结构是否按索引隔离（logger 的 `port[i]`）。
- **J2/J3 环收回发数量不一致（发多收少）**：多为缓冲/环路时序或丢字节，结合 `rx_bytes/drop_lines` 判断。
- **屏幕不亮、背光不亮但没重启**：UI 任务创建失败（内部 RAM 不足）或卡死。查串口有没有 `ui ready` / `ui task create failed`；任务须在 BLE/WiFi 之前创建并检查返回值。
- **BLE 广播起不来 / `adv set fields failed: 4`**：广播数据超 31 字节，去掉广播包里的 128 位 UUID，只留 flags+名称。
- **`httpd start failed`（Web 起不来但 AP 正常）**：内部 RAM 耗尽。确认 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，并检查 LCD i80 的 `max_transfer_bytes` 是否按整屏误设（应按刷新块，约 25KB 而非 150KB）。
- **手机连后 `header fields are too long`：HTTP 请求行/头超长（多由过多 cookie/长查询串导致），曾通过精简请求与服务端配置处理。
- **OTA 上传后安装失败**：`/api/ota` 的 `msg` 会带 `esp_err_to_name`。`ESP_ERR_OTA_VALIDATE_FAILED` 说明上传的 bin 不完整或不是本工程产物（app 偏移必须 0x20000、`PROJECT_VER` 一致）；`no OTA slot` 说明分区表没烧全（漏了 `ota_data_initial.bin`）。
- **新固件起来后又被"回滚"**：新镜像启动后 30s 内没跑到 `app_ota_tick()` 确认（例如开机会崩或任务被饿死），bootloader 会退回旧槽。查串口 `ota: slot running=... pending=1` 与是否能正常打印 `ui ready`。
- **日志没有自动清理**：`/api/storage` 的 `total_mb` 为 0 说明卡没挂上；`cleaned` 不增长说明占用还没到阈值（或手动清日志后 logger 仍在忙，请求会挂起到帧间空闲）。
- **Modbus 模拟从站不回包**：确认通道已打开（主站/从站都复用 logger 的 UART）、从站地址与请求一致、CRC 正确；主站规则若与从站同通道会被配置校验拒绝。

---

## 14. 无屏版本（后续目标）

- 去掉 LVGL/触摸相关初始化与 `ui_task`，可进一步释放内存与 Flash。
- 配置全部走 Web：串口参数、校时（含「用手机时间」）、分段、文件下载、TCP/BLE 透传——这些 API 已具备，保留即可。
- 首次无屏配网/访问：默认仍用 SoftAP `UART-LOG` / `12345678`，用户连上后在 Web 改参数；配置统一落 NVS。
- 建议保持 `board.h`、`app_logger`、`app_bridge`、`app_httpd`、`app_wifi`、`app_settings`、`app_time`、`app_sd` 与有无屏幕解耦，UI 仅作为可选上层。

---

## 15. 开发约定

- 优先复用 ESP-IDF 官方示例与成熟开源组件，不重复造轮子；改动保持最小、风格与现有代码一致。
- 不在代码里加版权/协议头；不擅自 git commit / 建分支（除非用户明确要求）。
- 配置一律持久化到 NVS；跨任务操作通过锁（`s_lock` / `app_fs_lock`）与异步请求标志，不在他任务上下文直接操作硬件/文件系统。
- 改结构体（尤其 NVS blob）记得同步 magic/version；改网页记得同步 `WEB_PAGE`；加屏幕汉字先进 `symbols.txt`。
- 验证路径：先针对性跑相关功能，再整体烧录实测；串口日志是主要判据。
- 主机单元测试：`python tests/unit/run.py`（原生 gcc 编译 `app_modbus.c` / `app_modbus_codec.c` / `app_parse.c`，覆盖 CRC、fc 01–06/0F/10 从站应答、异常码、主站组帧、DL/T645、CJ/T188、ASCII、auto 判别）。

---

_最后一次固件状态：BLE/WiFi 共存正常，Web、TCP/BLE 透传、文件浏览/下载、多路实时流、手机校时均已打通；已修复「屏幕不亮/BLE广播超长/httpd 内存不足」三联问题（UI 任务提前创建、广播去掉 128UUID、启用 PSRAM 给 WiFi/lwIP、缩小 i80 预留）。设备时钟已校准。_

_v2.0.0 新增：OTA 三槽升级（SD 暂存 + 断电安全 + PENDING_VERIFY 健康窗口 + 手动回滚）、按通道的存储阈值清理、每路可配日志协议解析（Modbus RTU/ASCII、DL/T645-2007、CJ/T188、ASCII 文本）、Modbus RTU 从站/主站模拟（复用 UART 与 logger 出站队列）。主机单元测试 88 项通过；整机功能待接板（COM3）实测。_
