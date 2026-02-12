# Tab5（ESP32‑P4）在 SD 卡加载中文 TTF 的踩坑记录（LVGL tiny_ttf）

本文记录在 `M5Tab5-UserDemo` 技术栈下，让 IRC 界面能显示中文（从 SD 卡加载 `font.ttf`）时遇到的关键坑点与最终可工作的配置。

## 目标
- 字体文件放在 SD 卡根目录：`/sd/font.ttf`（在 Windows 上就是 SD 盘符根目录的 `font.ttf`）
- 运行时从 SD 挂载点加载（不把 24MB 字体烧进 flash）
- UI 文本控件（如 TextArea）能正常显示中文

## 关键结论（最重要）

### 1) `lv_conf.h` 不一定会生效：要看 `CONFIG_LV_CONF_SKIP`
在本 Demo 中，`M5Tab5-UserDemo/platforms/tab5/sdkconfig` 默认是：
- `CONFIG_LV_CONF_SKIP=y`

这意味着 **LVGL 会走 Kconfig 配置**，而不是你在仓库里改 `M5Tab5-UserDemo/lv_conf.h` 就能生效。

所以要打开 tiny_ttf / 文件系统支持，必须改 `sdkconfig` / `sdkconfig.defaults`。

### 2) 必须打开 tiny_ttf + 文件加载 + stdio 文件系统
需要确保（Kconfig）：
- `CONFIG_LV_USE_TINY_TTF=y`
- `CONFIG_LV_TINY_TTF_FILE_SUPPORT=y`
- `CONFIG_LV_USE_FS_STDIO=y`
- `CONFIG_LV_FS_STDIO_LETTER=83`（ASCII 码 83 对应 `'S'`）

对应文件：
- `M5Tab5-UserDemo/platforms/tab5/sdkconfig`
- `M5Tab5-UserDemo/platforms/tab5/sdkconfig.defaults`

没有 `LV_USE_FS_STDIO`，`lv_tiny_ttf_create_file("S:/...")` 这类路径不会工作。

### 3) tiny_ttf 头文件 include 路径（LVGL v9）
本项目使用的 LVGL 在 `dependencies/lvgl/` 下，tiny_ttf 头文件在：
- `dependencies/lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h`

因此推荐 include：
- `#include <src/libs/tiny_ttf/lv_tiny_ttf.h>`

而不是 `#include <lv_tiny_ttf.h>`（容易找不到）。

### 4) 路径规则：FS_STDIO 的盘符 + “/sd” 挂载点
我们把 SD 挂载到：
- `/sd`

而 LVGL FS_STDIO 使用盘符（这里设为 `S`），所以 tiny_ttf 加载用：
- `S:/sd/font.ttf`

注意：这和 `fopen("/sd/font.ttf")` 是两个不同层的路径系统：
- `fopen` 走 libc/系统 VFS
- `S:/...` 走 LVGL 的 fs driver（stdio driver）

### 5) 大小写/文件名差异
实机上如果仍然报 `missing /sd/font.ttf`：
- 先确认 SD 是否挂载成功（串口有 mount 日志）
- 再确认根目录文件名是否真叫 `font.ttf`（有的系统/拷贝工具可能是 `FONT.TTF`）

当前实现里会尝试在 `/sd` 根目录 **大小写不敏感** 地查找 `font.ttf`，并在 UI 里打印 `/sd` 根目录文件列表，便于定位问题。

## 实际落地代码位置
- 字体加载逻辑：`M5Tab5-UserDemo/app/apps/utils/fonts/font_manager.cpp`
- SD 挂载：`M5Tab5-UserDemo/platforms/tab5/main/hal/hal_esp32.cpp`（`bsp_sdcard_init("/sd", ...)`）
- Kconfig：`M5Tab5-UserDemo/platforms/tab5/sdkconfig`、`M5Tab5-UserDemo/platforms/tab5/sdkconfig.defaults`

## 验收方式
1. SD 卡根目录放 `font.ttf`（本仓库也有一份：`sdcard/font.ttf`）
2. 启动后进入 IRC 界面
3. 串口出现 `loaded font ...`，并且 IRC TextArea 能显示中文消息

