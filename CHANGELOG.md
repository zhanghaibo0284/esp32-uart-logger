# CHANGELOG.md — ESP32-S3 串口记录仪 开发环境与变更全记录

> 用途：换账号 / 换设备 / 换开发工具时，依据本文件可完整恢复开发环境并无缝接续开发。
> 最后更新：2026-10-28（对应 HEAD `6c3b4dc`）
> 项目规格与踩坑记录见 [AGENTS.md](AGENTS.md)，两文件互补：AGENTS.md 讲「怎么干活」，本文件讲「干过什么、环境是什么」。

---

## 1. 仓库与分支状态

| 项 | 值 |
|---|---|
| 远程仓库 | `https://github.com/zhanghaibo0284/esp32-uart-logger.git` |
| 当前分支 | `main`（与 `origin/main` 同步，HEAD 已推送） |
| HEAD | `6c3b4dc5ba465c7aecd00f3cccd811b654c53587`（2026-10-28 14:10 +0800） |
| 工作区 | 干净（无未提交的代码改动；仅 4 个未跟踪的文档/备份文件，见 §8） |

提交历史（由新到旧）：

| Commit | 说明 |
|---|---|
| `6c3b4dc` | 文件管理增强 + 文件夹递归打包 + 日期目录 + TCP/BLE 透传改进（11 文件，+2014/−219） |
| `444ecba` | 修复屏幕文件列表超过 80 个时最新文件不可见 |
| `422c38a` | 重构日志接收路径为事件驱动 + 独立 disk writer，修复高速主从流量帧粘连/切分与透传阻塞 |
| `8dc74c2` | 新增屏幕按键控制与自动息屏、64 位计数溢出保护 |
| `e993127` | 归档 modbus-log-config-enhancement 并同步主 spec |
| `b8be5c7` | 新增 Modbus 解析、帧日志格式化与 config.json 配置管理 |
| `a8370f2` | ESP32-S3 多路串口记录仪首个可用版本 |

---

## 2. 开发环境（恢复环境时照此安装）

### 2.1 工具链

| 项 | 值 |
|---|---|
| ESP-IDF | **v5.3.1**，安装于 `C:\Espressif\frameworks\esp-idf-v5.3.1` |
| Python 环境 | `C:\Espressif\python_env\idf5.3_py3.11_env`（Python 3.11，含 esptool） |
| 编译入口 | `build_ascii.bat`（脚本内已 set IDF_PATH、PYTHONUTF8=1） |
| 下载口 | USB，**COM3**（VID_303A&PID_1001），app 偏移 `0x10000` |
| 串口波特率 | 监控 115200；烧录 460800 |

### 2.2 依赖包

| 包 | 版本 | 来源 |
|---|---|---|
| LVGL | **8.4.0** | `main/idf_component.yml` → `managed_components/lvgl__lvgl`（idf.py 自动拉取） |
| ESP-IDF | >=5.3.0（实际 5.3.1） | `main/idf_component.yml` |
| pyserial | （PC 端脚本用） | `pip install pyserial`，供 `mon_serial.py`/`read_boot.py`/`probe_tcp.py`/`cap_ble*.py`/`set_clock.py` |
| Node.js | （PC 端工具用） | 仅用于网页转义嵌入 WEB_PAGE、JS 语法检查，无项目依赖 |

### 2.3 工程路径硬约束

- **可工作工程必须在纯 ASCII 路径 `F:\esp32_uart_log`**。中文路径会导致 ldgen / objdump 编码损坏。
- 原始工程 `F:\B\硬件设计\esp32串口记录仪` 仅作备份，**绝不在该路径编译**。
- 正点原子资料盘（只读参考）：`E:\B\RDWorks\Embed\esp\正点原子\【正点原子】ESP32S3 BOX开发板\...`
- BLE 官方示例参考：`C:\Espressif\frameworks\esp-idf-v5.3.1\examples\bluetooth\nimble\bleprph\main\main.c`

### 2.4 硬件平台

正点原子 ESP32-S3 BOX：ESP32-S3（芯片修订 v0.2）、16MB Flash、8MB 八线 PSRAM、电容触摸屏（LVGL 8.4）、TF 卡走 SD-SPI（现用 64GB，exFAT 可挂载）、板载 I2C 扩展 IO（myiic + xl9555）。

引脚（改引脚只改 `main/board.h` 与 `main/app_sd.c`）：

| 功能 | GPIO |
|---|---|
| UART0 TX/RX（板载） | 43 / 44 |
| UART1 TX/RX（J2） | 5 / 6 |
| UART2 TX/RX（J3） | 8 / 18 |
| TF 卡 MOSI/MISO/SCLK/CS | 16 / 15 / 7 / 17 |

---

## 3. 最新一轮变更明细（HEAD `6c3b4dc`，2026-10-28）

主题：Web 端文件管理四项增强 + 配套后端改造。11 个文件，+2014/−219 行。

### 3.1 文件级变更

| 文件 | 变更摘要 |
|---|---|
| `main/app_httpd.c` | **核心改动**。① `files_get`：`records` 默认改为 `false`，根目录只列直接子项（日期文件夹+config.json），不再预遍历全卡；保留 `rec=1` 强制记录视图。② `collect_zip_all` 重写为**迭代式目录栈**（`zip_dir_stack_t`，栈缓冲 `heap_caps_malloc` 到 PSRAM，上限 `ZIP_DIR_STACK_MAX=128`），支持任意深度嵌套目录打包且不会爆 httpd 任务栈。③ `/api/zip` 的 `all=1` 去掉 `txt_only` 过滤，完整还原目录结构。④ Content-Disposition 文件名改为目录 basename（`<文件夹名>.zip`，根目录为 `sdcard.zip`）。⑤ httpd `stack_size` 8192→**12288**（修栈溢出），`SCAN_PATH_MAX=192`。 |
| `main/web_index.html` | ① 新增路径导航行：`#f-path` 输入框 + 跳转 / 根目录 / 全部记录视图按钮。② 目录项行内新增「下载」按钮（`data-dl-dir`，事件委托在 `#files`）。③ `downloadZip` 改用 `response.body.getReader()` 流式读取，实时显示已收字节数；新增 `parseFileName`（解析 Content-Disposition）、`saveBlob`、`downloadFolder`。④ `fstate` 增加 `records` 字段，`fileQuery` 支持 `rec=1`。⑤ 此前已加 `AbortController` 8 秒超时。改动后已重新转义嵌入 `app_httpd.c` 的 `WEB_PAGE`。 |
| `main/app_logger.c` / `app_logger.h` | 日志目录结构改为 `/sdcard/YYYY-MM-DD/UARTx/*.txt`（按日期分文件夹）；事件驱动接收 + 独立 disk writer 的配套调整。 |
| `main/app_sd.c` / `main/app_sd.h` | 新增 `app_sd_log_path` / `app_sd_ensure_log_dir` / `app_sd_date_folder`（日期目录生成与确保存在）。 |
| `main/app_bridge.c` | TCP/BLE 透传改进（与 logger 事件驱动路径对接）。 |
| `main/app_config.c` | config.json 配置管理增补。 |
| `main/app_modbus.c` | Modbus 解析配套更新。 |
| `main/app_settings.c` | 设置项配套更新。 |
| `main/app_ui.c` | 屏幕文件列表 >80 个时最新文件可见性修复的配套。 |

### 3.2 行为变化（对使用者可见）

- 打开 Web 文件页默认显示 `/sdcard` 直接子项（日期目录），秒开不卡顿；点「全部记录视图」才展开全部日志。
- 每个文件夹行有「下载」按钮 → 整文件夹（含所有嵌套层级）打包为 `<文件夹名>.zip` 下载，带实时进度。
- 路径输入框可手动跳转任意目录；面包屑同步。

---

## 4. 配置项明细

### 4.1 `partitions.csv`（自定义分区表）

```
nvs       data  nvs      0x9000   0x6000
phy_init  data  phy      0xf000   0x1000
factory   app   factory  0x10000  0x400000   # 4MB app
```

### 4.2 `sdkconfig.defaults`（基线配置，新机器 clone 后自动生成完整 sdkconfig）

| 类别 | 关键项 |
|---|---|
| 目标 | `CONFIG_IDF_TARGET="esp32s3"`，Flash 16MB，QIO（实际烧录用 dio），80MHz，CPU 240MHz |
| 控制台 | `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`（控制台走 USB Serial JTAG，不占 UART0） |
| PSRAM | `SPIRAM=y` + `SPIRAM_MODE_OCT=y` + `SPEED_80M`，`MALLOC_ALWAYSINTERNAL=16384`，`MALLOC_RESERVE_INTERNAL=32768`，**`SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`**（BLE+WiFi+httpd 共存的关键，省内部 RAM） |
| 分区 | `PARTITION_TABLE_CUSTOM=y`，文件名 `partitions.csv` |
| FATFS | `LFN_HEAP=y`，`MAX_LFN=255`，`API_ENCODING_UTF_8=y` |
| 系统 | 主任务栈 8192，`FREERTOS_HZ=1000` |
| LVGL | `LV_CONF_SKIP=y`，色深 16，内存 64KB，tabview/dropdown/btn/label/switch/spinbox/flex，dark theme，UTF-8 |

### 4.3 `sdkconfig` 中的蓝牙配置（不在 defaults，首次需确认）

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y        # Bluedroid 关闭，用 NimBLE
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_MAX_CONN_REATTEMPT=3
# 另有多项 CONFIG_BT_NIMBLE_* 内存精简（单连接、单 bond、ACL/msys 计数精简）
# 注意：findstr 直接搜 sdkconfig 可能因编码匹配不到，必要时用 Node 检索
```

### 4.4 代码内关键宏（`main/app_httpd.c`）

| 宏 | 值 | 说明 |
|---|---|---|
| `SCAN_PATH_MAX` | 192 | 目录扫描短路径缓冲（修栈溢出的关键） |
| `FILE_PATH_MAX` | 512 | 完整路径缓冲 |
| `FILE_NAME_MAX` | 96 | zip 条目名/相对路径缓冲（注意 -Werror：snprintf 截断会报错） |
| `ZIP_MAX` | 8000 | 单次 ZIP 最多打包条数（PSRAM 约 0.9MB） |
| `ZIP_DIR_STACK_MAX` | 128 | 迭代式目录栈深度上限（PSRAM 分配） |

httpd 服务器配置：`max_open_sockets=4`、`lru_purge_enable=true`、`stack_size=12288`、`max_uri_handlers=16`（当前注册 14 个 URI，新增接口须同步调大）、`recv_wait_timeout=10`、`send_wait_timeout=20`。

### 4.5 NVS 持久化布局

| Namespace | 键 | 内容 |
|---|---|---|
| `logger` | `ap_ssid` / `ap_pass` | SoftAP SSID/密码（默认 `UART-LOG`/`12345678`） |
| `logger` | （settings blob） | 分段时长 + 各串口参数 |
| `bridge` | （blob，magic `0x42524432`） | TCP/BLE 透传配置。**改结构体字段必须递增 magic** |

### 4.6 `main/CMakeLists.txt`

- SRCS 17 个源文件：main / app_settings / app_time / app_sd / app_logger / lvgl_port / app_ui / app_files / app_cmd / app_wifi / app_httpd / app_bridge / app_modbus / app_config / app_screen / dns_server / font_cn_16
- `REQUIRES BSP nvs_flash fatfs sdmmc esp_timer driver console esp_wifi esp_event esp_netif esp_http_server bt json`（**加新源文件要同步加进 SRCS**）
- 编译选项：`-finput-charset=UTF-8 -fexec-charset=UTF-8`，定义 `LV_LVGL_H_INCLUDE_SIMPLE`

---

## 5. 构建 / 烧录 / 监控命令速查

```bat
:: 编译
cmd /c "F:\esp32_uart_log\build_ascii.bat 2>&1"

:: 烧录（在 build 目录）
cd /d F:\esp32_uart_log\build
C:\Espressif\python_env\idf5.3_py3.11_env\Scripts\python.exe -m esptool ^
  --chip esp32s3 -p COM3 -b 460800 --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m @flash_args

:: 监控
python mon_serial.py     :: 持续读 25 秒
python read_boot.py      :: 拉 RTS 复位后读 7 秒（抓启动序列）
```

**监控坑**：pyserial `Serial("COM3")` 会拉 RTS 把设备按在复位态；须用 .NET `SerialPort` 并在 `.Open()` 之前设 `DtrEnable=$false; RtsEnable=$false`。

**WiFi 测试坑**：`netsh wlan connect name="UART-LOG"` 后 Windows 约 30 秒会自动切回家用路由器，HTTP 测试须在同一 shell 命令内紧跟 connect 执行。判据：`ping 192.168.4.1` 的 TTL=64 才是真连上设备（TTL=251 是本机虚拟网卡在应答）。

---

## 6. 功能完成度总览（当前固件状态：全部已实测通过）

| 功能 | 状态 |
|---|---|
| LVGL 六标签页 UI（状态/串口/时钟/日志/文件/WIFI） | ✅ 正常 |
| 三路串口独立配置 + 日志落盘（毫秒时间戳 + HEX） | ✅ 正常，事件驱动 + 独立 disk writer |
| 日志目录 `/sdcard/YYYY-MM-DD/UARTx/*.txt` | ✅ 已切换到日期目录结构 |
| 手动/手机时间校时 | ✅ 正常 |
| 屏幕端文件浏览器（>80 文件可见性已修复） | ✅ 正常 |
| Web：参数配置/校时/文件浏览/下载/ZIP/多路实时流 | ✅ 正常 |
| Web 文件夹递归打包下载（任意嵌套深度 + 进度提示） | ✅ 本轮新增并实测通过 |
| Web 路径切换控件（输入框 + 跳转/根目录/全部记录视图） | ✅ 本轮新增并实测通过 |
| WiFi SoftAP + captive portal DNS | ✅ 正常 |
| WiFi TCP 透传（每路一个端口，默认 8081/8082/8083） | ✅ 正常 |
| BLE NUS 透传（NimBLE，与 SoftAP 共存） | ✅ 正常 |
| USB Serial JTAG 控制台命令 | ✅ 正常 |
| Modbus 解析 + 帧日志格式化 + config.json | ✅ 已归档 spec |
| 屏幕按键 + 自动息屏 + 64 位计数溢出保护 | ✅ 已合入 |

---

## 7. OpenSpec 工作流状态

- `openspec/specs/`：config-management、frame-logging、modbus-parsing、screen-power、traffic-statistics（5 份主 spec）
- `openspec/changes/archive/`：`2026-10-07-key-screen-counter`、`2026-10-07-modbus-log-config-enhancement`（均已归档）
- **当前无进行中的 OpenSpec change**。本轮文件管理增强未走 OpenSpec 流程。

---

## 8. 未跟踪文件（工作区内，未提交 Git）

| 文件 | 说明 |
|---|---|
| `U盘模式可行性评估报告.md` | U 盘（USB MSC）模式的前期评估 |
| `Web文件全量展示优化方案.md` | Web 文件列表全量展示的方案文档（本轮已部分落地） |
| `date_folder_scheme.md` | 日期目录方案设计文档（本轮已落地） |
| `esp32-uart-logger-src-444ecba-20261026.zip` | `444ecba` 版本的源码备份压缩包 |
| `项目代码结构分析报告.md` | 已跟踪（在 Git 内） |

> 这些文档属于设计资料/备份，是否需要入库由后续决定；源码备份 zip 建议不要入库。

---

## 9. 未完成的功能点与后续待办

### 9.1 规划中：无屏幕版本（AGENTS.md §14）

- 去掉 LVGL/触摸初始化与 `ui_task`，释放内存与 Flash。
- 配置全部走 Web（串口参数/校时/分段/文件下载/TCP/BLE 透传的 API 已具备）。
- 首次配网仍用默认 SoftAP `UART-LOG`/`12345678`。
- 要求 `board.h`、`app_logger`、`app_bridge`、`app_httpd`、`app_wifi`、`app_settings`、`app_time`、`app_sd` 与 UI 解耦（目前耦合度已较低）。

### 9.2 待办清单（按建议优先级）

1. **无屏版本裁剪**：条件编译或独立工程去掉 LVGL 依赖（UI 仅作可选上层）。
2. **U 盘模式**：依据 `U盘模式可行性评估报告.md` 决定是否实现 USB MSC（TF 卡通过 USB 暴露给电脑）。
3. **Web 文件全量展示的剩余优化**：依据 `Web文件全量展示优化方案.md` 核对未落地项。
4. **文档入库决策**：§8 中三份 md 是否提交 Git。
5. **`/api/files` 的 `max_uri_handlers`**：新增 Web 接口时注意 16 上限（当前已用 14）。
6. **回归测试基线**：`/api/files` 根目录连续刷新 ≥10 次不重启（本轮已验证 12 次全 200）。

---

## 10. 新环境恢复步骤（Checklist）

1. 安装 ESP-IDF **v5.3.1** 到 `C:\Espressif\frameworks\esp-idf-v5.3.1`（保持路径一致，脚本内写死）。
2. `git clone https://github.com/zhanghaibo0284/esp32-uart-logger.git F:\esp32_uart_log`（**必须纯 ASCII 路径**）。
3. 安装 Python 依赖：`pip install pyserial`（PC 端脚本用）。
4. 首次编译：`cmd /c "F:\esp32_uart_log\build_ascii.bat 2>&1"` —— idf.py 会依据 `sdkconfig.defaults` 生成 `sdkconfig`，并自动拉取 `managed_components/lvgl__lvgl`（8.4.0）。
5. 核对 sdkconfig 蓝牙项（§4.3）已生效；若缺失，从备份 sdkconfig 拷贝或 menuconfig 开启 NimBLE。
6. 烧录（§5），`read_boot.py` 抓启动序列确认无 `Invalid mbox` / `BLE_INIT` 报错。
7. PC 连 `UART-LOG`，浏览器开 `http://192.168.4.1` 验证 Web；ping TTL=64 确认真连接。
8. 通读 [AGENTS.md](AGENTS.md) 的「关键路径警告」「初始化顺序」「修改网页/屏幕字体的硬约束」三节再动手改代码。

---

## 11. 不可违背的硬约束（改代码前必读，摘自 AGENTS.md）

1. **初始化顺序不可乱**：nvs → time/settings → iic/xl9555 → SD → logger → **ui_task（core1，须检查返回值）** → **BLE（必须在 WiFi 前）** → cmd → **WiFi/SoftAP** → httpd → **TCP 透传（必须在网络栈后）**。
2. **BLE 广播数据 ≤31 字节**：只放 flags + 完整名称，不要把 128 位 UUID 放进广播包。
3. **改 `web_index.html` 后必须重新转义嵌入 `app_httpd.c` 的 `WEB_PAGE`**（Node 脚本转义 `\`→`\\`、`"`→`\"`、换行→`\r\n`，替换 `WEB_PAGE[] = "` 与下一个 `";` 之间内容，再 `node --check` 抽出的 `<script>`）。
4. **屏幕 UI 汉字必须先在 `main/symbols.txt` 内**，否则显示小方块；新汉字需重新生成 `font_cn_16.c`。
5. **改 NVS blob 结构体必须递增 magic**（bridge 当前 `0x42524432`）。
6. 不在中文路径编译；不擅自 git commit / 建分支（除非用户明确要求）；不在代码里加版权/协议头。
7. 跨任务操作走锁（`s_lock`/`app_fs_lock`）与异步请求标志，不在他任务上下文直接操作硬件/文件系统。
