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
4. app_sd_mount()
5. app_logger_start()
6. 创建 ui_task（xTaskCreatePinnedToCore, core=1）★须在 BLE/WiFi 之前，并检查返回值
7. app_bridge_ble_init()     // ★BLE 控制器 + NimBLE 主机，必须在 WiFi 之前
8. app_cmd_start()
9. (可选) app_logger_request_sd_test()
10. app_wifi_start()         // esp_netif_init / 默认事件循环 / SoftAP
11. 若 wifi.up → app_httpd_start()
12. app_bridge_tcp_resume()  // ★TCP 透传，必须在网络栈就绪之后
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

`flash_args` 内含：`0x0 bootloader.bin`、`0x8000 partition-table.bin`、`0x10000 uart_logger.bin`。

**串口监控**（115200）：

- `cmd /c "cd /d F:\esp32_uart_log && python mon_serial.py"` — 持续读 25 秒
- `... python read_boot.py"` — 拉 RTS 复位后读 7 秒（抓启动序列）

**重要环境排错经验**：测 Web 前先确认电脑真的连在记录仪 AP（`UART-LOG`）上，而不是家里路由器。曾出现电脑连家里路由器时 `ping 192.168.4.1` 仍通但 **TTL=251**（正常 ESP32 lwIP 为 64），那是本机虚拟网卡/VPN 在应答，流量没到设备，表现为网页「failure」、TCP 80 超时。判据：TTL 是否为 64、`netsh wlan show interfaces` 的 SSID。

---

## 8. 功能模块说明

### app_settings
- 结构：`segment_min`（日志分段，分钟）+ 每路 `port_setting_t`（baud / data_bits / stop_bits / parity / enabled）。
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
- 每行格式：`时间戳: <HEX...>`，**毫秒级时间戳**，数据体一律大写 16 进制（如 `55 AA 01`）。
- `app_view_t` 汇总给 UI / 命令行：每路 open/error/rx_bytes/drop_lines/tx_frames/tx_bytes/sending/param/file_name，以及 SD 状态和最近一帧 HEX。
- 周期发送：`app_logger_set_periodic_tx(port, interval_ms)` / 停止；单次测试 `request_tx_test`（发 `55 AA 01 02 03`）。
- 注入：`app_logger_inject(port, data, len)`（控制台 inject 用）。
- 实时流：`app_logger_live_get(index, since, ..., &next)` 维护每路环形缓冲与游标，供 `/api/live` 轮询（支持多路同时）。
- 请求类接口（reload/remount/sd_test）通过标志位异步在 logger 上下文执行，避免跨任务直接操作。

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

服务器配置：`max_open_sockets=4`、`lru_purge_enable=true`、`stack_size=8192`、`max_uri_handlers=16`、`recv_wait_timeout=10`、`send_wait_timeout=20`。

| 方法 | 路径 | 作用 |
|------|------|------|
| GET | `/` | 返回内嵌网页 |
| GET | `/api/status` | 时间、SD/剩余、SSID、三路串口状态与参数 |
| GET | `/api/files?dir=<path>` | 列目录（区分文件/子目录，供浏览与下载） |
| GET | `/api/port?index=&baud=&bits=&stop=&parity=&on=` | 设置某路参数并开关 |
| GET | `/api/time?y=&mo=&d=&h=&mi=&s=` | 手动校时，返回 `{ok,time}` |
| GET | `/dl?path=<file>` | 下载单个文件 |
| GET | `/api/zip?dir=<dir>&names=a|b|c` 或 `&all=1` | 打包下载选中 / 全选 |
| GET | `/api/live?<i>=<cursor>...` | 多路实时数据流（增量，带 next 游标） |
| GET | `/api/bridge` | TCP/BLE 透传状态 |
| GET | `/api/bridge_tcp?index=&on=&port=` | 开关某路 TCP 透传及端口 |
| GET | `/api/bridge_ble?index=&on=&name=<urlenc>` | 开关 BLE 广播、绑定串口、改名 |

网页主要交互：串口卡片（参数+开关）、串口透传卡片（TCP 端口 / BLE 绑定与名称）、实时数据（U0/J2/J3 勾选 + 清空）、校时（手填 + 「用手机时间」）、日志文件（面包屑进目录、多选、全选、下载选中/全部）。

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
phy_init  data  phy      0xf000  0x1000
factory   app   factory  0x10000 0x400000   # 4MB app
```

`sdkconfig.defaults` 要点：目标 esp32s3；Flash 16MB；240MHz；8MB 八线 PSRAM（`SPIRAM_MALLOC_RESERVE_INTERNAL=32768`，给 DMA/内部分配预留；**`SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，WiFi/lwIP 优先用 PSRAM 以省内部 RAM，是 BLE+WiFi+httpd 共存的关键**）；FATFS LFN heap、UTF-8；LVGL（8.4，UTF-8，tabview/dropdown/btn/label/switch/spinbox/flex/dark theme）；主任务栈 8192。

蓝牙在 sdkconfig 中：`CONFIG_BT_ENABLED=y`、`CONFIG_BT_NIMBLE_ENABLED=y`（Bluedroid 关闭）。注意：用 findstr 在 sdkconfig 里直接搜可能因编码匹配不到，需要时用 Node/其它方式检索。

`main/CMakeLists.txt` 的 `REQUIRES` 含：`BSP nvs_flash fatfs sdmmc esp_timer driver console esp_wifi esp_event esp_netif esp_http_server bt`。加新源文件要同时加进 SRCS 列表。编译选项：`-finput-charset=UTF-8 -fexec-charset=UTF-8`。

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
- 改结构体（尤其 NVS blob）记得同步 magic；改网页记得同步 `WEB_PAGE`；加屏幕汉字先进 `symbols.txt`。
- 验证路径：先针对性跑相关功能，再整体烧录实测；串口日志是主要判据。

---

_最后一次固件状态：BLE/WiFi 共存正常，Web、TCP/BLE 透传、文件浏览/下载、多路实时流、手机校时均已打通；已修复“屏幕不亮/BLE广播超长/httpd 内存不足”三联问题（UI 任务提前创建、广播去掉 128UUID、启用 PSRAM 给 WiFi/lwIP、缩小 i80 预留）。设备时钟已校准。_
