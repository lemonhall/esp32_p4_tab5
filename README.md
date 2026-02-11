# ESP32-P4 Tab5（M5Stack）开发工作区

这是一个面向 **M5Stack Tab5（ESP32‑P4 + ESP32‑C6）** 的本地开发工作区（Windows 11 / PowerShell），用于：
- 沉淀调研资料与开发规范
- 基于上游 `M5Tab5-UserDemo`（出厂 Demo 源码）做二次开发与刷机验证

> AI/自动化协作说明见：`AGENTS.md`

## 目录与内容
- 深度调研（重点看 Windows 开发环境与硬件差异）：`ESP32P4-Tab5-Deep-Research.md`、`ESP32P4-Tab5-Deep-Research.pdf`
- 上游 Demo（已 vendored 到本仓库，作为普通目录参与提交）：`M5Tab5-UserDemo/`
  - P4 固件工程入口：`M5Tab5-UserDemo/platforms/tab5/`
  - C6 Wi‑Fi 固件（二进制）：`M5Tab5-UserDemo/platforms/tab5/wifi_c6_fw/`

## 技术栈速览（沿用 Demo）
- ESP‑IDF：建议 `v5.4.2`
- UI：LVGL（Demo 固定拉取 `v9.2.2` 到 `M5Tab5-UserDemo/dependencies/lvgl`）
- 应用/组件：`smooth_ui_toolkit`、`mooncake`、`mooncake_log`
- 组件依赖：ESP‑IDF Component Manager（`idf_component.yml` + `dependencies.lock`）
- P4↔C6：`esp_hosted` + `esp_wifi_remote`（Demo 依赖）

## 开发环境（Windows 11 / PowerShell）
本工作区不使用 WSL2 开发（USB/串口坑多）。

本机 ESP‑IDF 由 EIM 安装，并已迁移到：
- `D:\esp\v5.4.2\esp-idf`
- `D:\Espressif\tools`

激活环境：
```powershell
. "D:\Espressif\tools\Microsoft.v5.4.2.PowerShell_profile.ps1"
idf.py --version
```

（可选）中国大陆网络环境：
```powershell
$env:HTTP_PROXY='http://127.0.0.1:7897'; $env:HTTPS_PROXY='http://127.0.0.1:7897'
```

## 快速开始：编译 + 刷机（P4）

### 1) 拉取 Demo 的依赖源码（只需一次）
```powershell
python .\M5Tab5-UserDemo\fetch_repos.py
```

### 2) 编译
```powershell
. "D:\Espressif\tools\Microsoft.v5.4.2.PowerShell_profile.ps1"
cd .\M5Tab5-UserDemo\platforms\tab5
idf.py build
```

### 3) 进入下载模式
USB 连接后 **长按 Reset，直到绿灯快速闪烁** 再松开。

### 4) 找对串口并刷机
Tab5 可能同时出现多个 COM 口；经验上 **P4 的下载/日志口通常是 `USB 串行设备 (COM6)`（USB‑Serial/JTAG）**，而不是 `CH340 (COM5)`。

查看端口：
```powershell
Get-CimInstance Win32_PnPEntity | ? { $_.Name -match '\\(COM\\d+\\)' } | select Name
```

刷机 + 监视（示例用 `COM6`）：
```powershell
. "D:\Espressif\tools\Microsoft.v5.4.2.PowerShell_profile.ps1"
cd .\M5Tab5-UserDemo\platforms\tab5
idf.py -p COM6 -b 460800 flash
idf.py -p COM6 monitor
```

## 如何确认“自己编译的固件真的生效”
为避免“刷了但看不出来”，我们在 Launcher 界面左上角加了 build tag（形如 `LEMON <__DATE__> <__TIME__>`）：
- `M5Tab5-UserDemo/app/apps/app_launcher/view/view.cpp`
- `M5Tab5-UserDemo/app/apps/app_launcher/view/view.h`

## 恢复出厂（提示）
如果要恢复官方固件，通常使用 M5Burner 按官方流程刷回出厂固件（建议你在做大改前先备份自己板子的 flash）。

## 许可与来源
- `M5Tab5-UserDemo/` 来源于 M5Stack 开源项目（其目录内包含原项目的 `LICENSE`）。

