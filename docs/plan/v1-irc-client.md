# v1 Plan: Tab5 IRC Client

## Goal
实现 Tab5 竖屏 IRC Client（v1）：可配网 → 可连接 `irc.lemonhall.me:6667` → 自动加入 `#tab5` → 竖屏滚动显示（含中文）→ 一键发送固定测试消息，并保留“语音输入”按钮占位。

## PRD Trace
- `docs/prd/PRD-0001-irc-client.md`
- REQ-0001-001 / 002 / 003 / 004 / 005 / 006 / 007

## Scope（做什么）
- Wi‑Fi：SoftAP + Web 配置页写入 NVS；STA 成功后关闭 AP；掉线超时回到 AP。
- Fonts：从 SD 卡 `/sd/font.ttf` 运行时加载中文字体（tiny_ttf + stdio fs），失败则降级。
- IRC：明文 TCP；NICK 自动生成（≤9）并冲突重试；握手（NICK/USER）→ `JOIN #tab5`；处理 `PING`。
- UI：在 Launcher 中新增一个“IRC”入口面板，打开全屏窗口显示消息；按钮“发送测试”“语音(占位)”。

## Out of Scope（不做什么）
- TLS/SASL/NickServ
- 多服务器/多频道管理
- 屏幕文本输入/中文输入法
- IRC 颜色码/表情完整渲染

## Acceptance（硬口径）
1) 固件刷入后，首次无配置时出现 SoftAP；浏览器打开 `http://192.168.4.1` 能提交 SSID/密码；提交后串口出现保存与 STA 连接日志。（REQ-0001-001）
2) STA 成功后 AP 关闭；断网持续 30s 后 AP 重新出现。（REQ-0001-002）
3) 打开 IRC 窗口后：能连到 `irc.lemonhall.me:6667`；完成握手并自动 `JOIN #tab5`；串口持续出现 PING/PONG 或心跳日志且连接不断开 ≥ 5 分钟。（REQ-0001-003/005）
4) SD 卡根目录放 `font.ttf` 后，窗口能显示中文消息；无 SD 或缺文件会提示并不崩溃。（REQ-0001-004）
5) 点击“发送测试”按钮会向 `#tab5` 发固定内容；串口打印要发送的 `PRIVMSG`。（REQ-0001-006）
6) “语音”按钮可点，不崩溃。（REQ-0001-007）

## Files（预计改动）
- `M5Tab5-UserDemo/lv_conf.h`（开启 tiny_ttf + stdio fs）
- `M5Tab5-UserDemo/platforms/tab5/main/hal/components/hal_wifi.cpp`（改为配网+STA+自愈）
- `M5Tab5-UserDemo/platforms/tab5/main/hal/hal_esp32.h` / `.../hal_esp32.cpp`（新增 wifi manager/状态接口；SD mount 行为调整）
- `M5Tab5-UserDemo/app/hal/hal.h`（抽象出 Wi‑Fi/SD/字体所需最小接口）
- `M5Tab5-UserDemo/platforms/desktop/hal/hal_desktop.*`（补齐接口以保证桌面构建不破）
- `M5Tab5-UserDemo/app/apps/app_launcher/view/view.h` / `.../view.cpp`（新增 PanelIrc）
- `M5Tab5-UserDemo/app/apps/app_launcher/view/panel_irc.cpp`（新文件：IRC UI）
- `M5Tab5-UserDemo/app/apps/utils/net/irc_client.*`（新文件：IRC 协议最小实现）
- `tools/e2e/Wait-SerialPattern.ps1`（新文件：E2E 串口日志断言）

## Steps（Strict）
1) **Red（文档/脚本先行）**：添加 `tools/e2e/Wait-SerialPattern.ps1`，先在当前固件上运行并预期失败（找不到新日志关键字）。
2) **Green（Wi‑Fi）**：实现 REQ-0001-001/002，补齐串口日志关键字。
3) **Green（Fonts）**：开启 tiny_ttf + stdio fs；实现 SD `font.ttf` 加载与降级提示。
4) **Green（IRC）**：实现 socket 连接、NICK/USER/JOIN、PING/PONG、消息解析入队列。
5) **Green（UI）**：新增 IRC 面板与窗口；消息滚动显示；按钮触发固定 PRIVMSG；语音按钮占位。
6) **E2E**：刷机后运行 `tools/e2e/Wait-SerialPattern.ps1 -Port COM6 ...` 验证关键日志链路；并在窗口里验证中文显示。
7) **Review**：更新 `docs/plan/v1-index.md` 状态；记录差异。
8) **Ship**：`git add -A ; git commit -m "v1: feat: irc client skeleton" ; git push`

## Risks
- `esp_wifi_remote` 环境下 AP/STA 切换的事件顺序与稳定性：需要充分打日志并做重试/超时。
- tiny_ttf + LVGL fs：需要正确启用 `LV_USE_FS_STDIO` 并确保 SD 在使用期间保持 mount。
- IRC 消息量与 UI 性能：TextArea 累积过大可能导致卡顿，需要设置最大长度并裁剪。

