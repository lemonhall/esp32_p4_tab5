# Agent Notes（ESP32-P4 / M5Stack Tab5 开发工作区）

本仓库是 Tab5（ESP32‑P4 + ESP32‑C6）在 **Windows 11 + PowerShell** 下的开发工作区：包含一份深度调研报告，以及上游 `M5Tab5-UserDemo` 源码（我们会沿用其技术栈做二次开发）。

## 项目概览
- **目标硬件**：M5Stack Tab5（主控 ESP32‑P4；无线由 ESP32‑C6 承担）
- **主要技术栈**：
  - **ESP-IDF**：以 `v5.4.2` 为基线（与 `M5Tab5-UserDemo` 对齐）
  - **UI**：LVGL（Demo 拉取 `v9.2.2` 到 `M5Tab5-UserDemo/dependencies/lvgl`）
  - **应用框架/组件**：`smooth_ui_toolkit`、`mooncake`、`mooncake_log`（位于 `M5Tab5-UserDemo/dependencies/`）
  - **IDF 组件管理**：ESP-IDF Component Manager（`M5Tab5-UserDemo/platforms/tab5/main/idf_component.yml` + `dependencies.lock`）
  - **无线协作（P4↔C6）**：`espressif/esp_hosted` + `espressif/esp_wifi_remote`（由 Demo 依赖声明）

## 重要文件
- 深度调研报告（先看这个再做硬件相关改动）：`ESP32P4-Tab5-Deep-Research.md`
- 同名 PDF：`ESP32P4-Tab5-Deep-Research.pdf`
- 上游 Demo 源码根目录：`M5Tab5-UserDemo/`
- P4 固件工程（IDF build/flash 入口）：`M5Tab5-UserDemo/platforms/tab5/`
- C6 Wi‑Fi 固件（二进制）：`M5Tab5-UserDemo/platforms/tab5/wifi_c6_fw/`

## 快速命令（Windows / PowerShell）
说明：本项目 **不使用 WSL2** 做开发/构建/烧录（坑多且 USB 透传不稳定）。

### 0) 代理（可选）
中国大陆网络环境下，建议在当前 PowerShell 会话先设置：
```powershell
$env:HTTP_PROXY='http://127.0.0.1:7897'; $env:HTTPS_PROXY='http://127.0.0.1:7897'
```

### 1) 激活 ESP-IDF 环境（必须）
本机 ESP-IDF 由 EIM 安装并已迁移到 D 盘：
- ESP-IDF：`D:\esp\v5.4.2\esp-idf`
- Tools：`D:\Espressif\tools`

激活命令：
```powershell
. "D:\Espressif\tools\Microsoft.v5.4.2.PowerShell_profile.ps1"
idf.py --version
```

### 2) 拉取 Demo 依赖仓库（只需一次）
```powershell
python .\M5Tab5-UserDemo\fetch_repos.py
```

### 3) 编译 Tab5 固件（P4）
```powershell
cd .\M5Tab5-UserDemo\platforms\tab5
idf.py build
```

### 4) 烧录 + 串口监视（P4）
先找出正确端口（Tab5 通常同时出现多个 COM 口）：
```powershell
Get-CimInstance Win32_PnPEntity | ? { $_.Name -match '\(COM\d+\)' } | select Name
```
经验上：
- **`COM6`（USB 串行设备 / USB-Serial/JTAG）**是 ESP32‑P4 下载/日志口
- **`COM5`（CH340）**多半不是 P4（可能连到 C6/其他接口），不要拿它刷 P4

进入下载模式：USB 连接后 **长按 Reset 直到绿灯快速闪烁** 再松开。

烧录（示例用 `COM6`）：
```powershell
cd .\M5Tab5-UserDemo\platforms\tab5
idf.py -p COM6 -b 460800 flash
idf.py -p COM6 monitor
```

### 5) 常用清理
```powershell
idf.py fullclean
```

## 架构速览（M5Tab5-UserDemo）
嵌入式入口在 `M5Tab5-UserDemo/platforms/tab5/main/app_main.cpp`，主循环跑应用层：

```
app_main()
  -> hal::Inject(HalEsp32)
  -> app::Init / app::Update
  -> LauncherView（LVGL UI）+ panels（各外设 demo）
```

代码分层大致如下：
- 应用层（跨平台/业务/UI）：`M5Tab5-UserDemo/app/`
- P4 平台入口与 HAL：`M5Tab5-UserDemo/platforms/tab5/main/`
- 板级支持包（屏幕/触控/背光/外设初始化等）：`M5Tab5-UserDemo/platforms/tab5/components/m5stack_tab5/`
- 额外依赖源码（非 component manager）：`M5Tab5-UserDemo/dependencies/`
- 通过 component manager 拉取的依赖：构建时进入 `M5Tab5-UserDemo/platforms/tab5/managed_components/`（自动生成）

## “自定义已生效”的可视化标识
为了区分“自己编译刷入”与“出厂固件”，我们在 Launcher 界面左上角加了 build tag：
- `M5Tab5-UserDemo/app/apps/app_launcher/view/view.cpp`
- 文案形如：`LEMON <__DATE__> <__TIME__>`

## 代码风格与约定
- C/C++ 格式：以 `M5Tab5-UserDemo/.clang-format` 为准
- 代码改动尽量集中在 `M5Tab5-UserDemo/app/` 或 `M5Tab5-UserDemo/platforms/tab5/main/`；除非明确需要，否则少动 BSP（`platforms/tab5/components/m5stack_tab5/`）
- PowerShell 连续命令用 `;` 分隔（不要用 `&&` 期望 bash 语义）

## 安全边界（必须遵守）
- **刷机是破坏性操作**：执行 `idf.py flash` / `erase_flash` 前先二次确认（尤其是板子里有重要数据时）。
- **不要提交 secrets**：Wi‑Fi 密码、token、证书、私钥、设备 token 等不得进 git。
- **不要“升级依赖试试”**：`idf.py update-dependencies` 会改 `dependencies.lock`，除非你就是要做升级验证，否则不要动；若要升级，必须说明原因并给出回滚方式。
- **危险文件操作要确认**：`Remove-Item -Recurse -Force`、大范围删除/覆盖前必须先问用户。

## 暗语（协作快捷指令）
为避免来回解释、也防止误刷，约定以下“暗语”：
- **“绿闪了”**：你已确认设备处于下载模式（绿灯快速闪烁），并授权我执行 `idf.py -p COM6 -b 460800 flash`（仍不包含 `erase_flash`，除非你明确说要擦除）。

## 验证策略（最小闭环）
- 代码改动后至少跑一次：`idf.py build`
- 需要硬件验证时：`idf.py -p COM6 flash ; idf.py -p COM6 monitor`

## 多份 AGENTS.md 覆盖规则
- 根目录 `AGENTS.md` 适用于整个工作区
- 若未来在子目录新增 `AGENTS.md`，则以离目标文件更近的规则为准（子目录覆盖父目录）
