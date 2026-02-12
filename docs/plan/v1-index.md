# v1 Index（Tab5 IRC Client）

## Scope（v1）
目标：交付一个可用的“竖屏 IRC client v1”，满足 `docs/prd/PRD-0001-irc-client.md` 的 REQ-0001-001~007。

## Milestones
- M1：Wi‑Fi 配网 + 自愈（REQ-0001-001,002）
- M2：中文字体加载（REQ-0001-004）
- M3：IRC 连接 + JOIN + PING/PONG + 显示（REQ-0001-003,005,006）
- M4：固定测试消息 + 语音按钮占位（REQ-0001-006,007）

## Plans
- `docs/plan/v1-irc-client.md`

## Traceability Matrix（Req → Plan → Verification）
| Req ID | Plan | Verification（命令/脚本/证据） | Status |
|---|---|---|---|
| REQ-0001-001 | v1-irc-client §Wi‑Fi | 串口日志 + `pwsh -File tools/e2e/Wait-SerialPattern.ps1 -Port COM6` | todo |
| REQ-0001-002 | v1-irc-client §Wi‑Fi | 串口日志 + 手动断网复现（30s 回 AP） | todo |
| REQ-0001-003 | v1-irc-client §IRC | 串口日志（`start nick=` / `nick in use, retry`） | todo |
| REQ-0001-004 | v1-irc-client §Fonts | 串口日志（`loaded /sd/font.ttf`）+ 屏幕显示中文 | todo |
| REQ-0001-005 | v1-irc-client §IRC | 串口日志（`connected irc.lemonhall.me:6667` / `join #tab5`） | todo |
| REQ-0001-006 | v1-irc-client §UI | 串口日志（`PRIVMSG #tab5 :hi from ...`）+ 屏幕滚动 | todo |
| REQ-0001-007 | v1-irc-client §UI | 点击 Voice 按钮弹 toast 且不崩溃 | todo |

## ECN Index
- （v1 暂无）

## Differences（愿景 vs 现实）
- （v1 结束时填写）
