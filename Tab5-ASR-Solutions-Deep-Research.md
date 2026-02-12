# TAB5（ESP32‑P4）语音输入：成熟 ASR 接入方案调研（百炼 Fun‑ASR / ISI‑NLS / 本地自建）

## Executive Summary
从你当前的崩溃日志（`mbedtls_ssl_setup returned -0x7F00`、`SSL_HANDSHAKE_FAILED` 后紧接着 `Stack protection fault`）看，**TAB5 设备端直连公网 `wss://...`（TLS + WebSocket + 流式音频）**属于高风险路径：TLS 连接的内存占用峰值、碎片化以及多任务并发，会把系统推到临界点，导致“偶发能跑、但不够稳定”。[1][2][3]

更成熟/工程化的方案通常是：**TAB5 只负责采音 + 局域网传输（尽量不做公网 TLS）→ 网关/服务器对接云端 ASR**（百炼或 ISI/NLS 均可），把 TLS、鉴权、断句/VAD、重试与日志隔离放到资源更充足的机器上。[4][5]

## Key Findings
- **百炼 Fun‑ASR realtime（DashScope）给出了完整 WebSocket 协议与参数**：使用 `wss://dashscope.aliyuncs.com/api-ws/v1/inference/`，发送 `run-task`（`streaming: duplex`）后推二进制音频帧，回调包含 `task-started`/`result-generated` 等；支持 `max_sentence_silence`、`semantic_punctuation_enabled`、`language_hints` 等参数，便于做“服务端断句/静音收句”。[4]
- **ISI（智能语音交互/NLS）属于传统语音产品线，文档对“流式音频帧大小/采样率/格式”更细**：明确 8k/16k、PCM 16bit mono，并在 SDK FAQ 中建议单次发送 1600/3200 bytes（典型对应 100ms 音频），更利于实现稳定的实时转写链路。[5]
- **`-0x7F00`（`MBEDTLS_ERR_SSL_ALLOC_FAILED`）在 ESP32 语境下通常就是 TLS 内部内存分配失败**，常见诱因是可用堆不足或最大连续块不足（碎片化）。[1]
- **ESP‑IDF 提供多项“降低 TLS 常驻内存”的配置**（动态缓冲、IN/OUT content len、分配策略等），但这些优化无法改变“公网 WSS = 高峰值 + 高抖动”的客观工程风险；如果要稳定，优先把 TLS 移到网关侧。[2][3]
- **可选的成熟替代**：在网关侧自建 ASR（如 FunASR）可完全绕开云端依赖与计费，但需要自持算力与运维，且实时效果/延迟需实测评估。[6][7]

## Detailed Analysis

### 1) 为什么你现在的“设备端直连 WSS”容易炸
你日志里出现两条线索经常一起出现：
1) **TLS 申请内存失败**（如 `mbedtls_ssl_setup returned -0x7F00`）→ 直接说明 TLS 初始化/缓冲分配在某个时刻拿不到足够内存（或最大连续块）。[1]  
2) **随后出现 Stack protection fault，回溯落到 newlib locks / `vfprintf`** → 这不一定是“printf 真的把栈撑爆”这么简单，更常见是：在高并发和频繁分配/释放后出现 **内存破坏/越界写/竞争条件**，最终在任意任务里触发栈金丝雀或锁结构断言。

在 TAB5 这类“UI(LVGL) + Wi‑Fi + I2S 录音 + WebSocket + TLS”的组合里，公网 WSS 让系统处于持续高压：
- TLS 握手、证书链验证、加解密都有明显的内存峰值（尤其当 heap 最大连续块被 UI/音频动态分配切碎时）。
- 网络抖动/重连会反复触发这些峰值，导致“偶发可用但不稳”。

结论：如果目标是“能长期稳定用”，**成熟方案应把 TLS 从设备端移走**。

### 2) 成熟方案优先级（推荐 → 备选）

#### 方案 A（最推荐）：TAB5 → 局域网网关 → 云端 ASR（百炼 Fun‑ASR 或 ISI/NLS）
**核心思想**：TAB5 只做“采音 + 局域网推流”，网关/服务器负责：TLS/WSS、鉴权、断句/VAD、重试、限流与日志。

TAB5 端最小闭环（建议）：
- 录音统一成：16kHz、16bit、mono PCM（TAB5 若是 48k 采样，网关或设备端做下采样均可；为了省算力，推荐网关侧做重采样）。
- 传输协议：到你局域网机器的 `ws://`（无 TLS）或 `http://`（chunked/分片上传均可）。
- 交互：按 `Voice` → 开始推流 → 网关“静音收句/超时收句” → 回传最终中文 → TAB5 Toast 显示 3–5 秒并发送 IRC。

网关对接云端有两条成熟路径：
- **网关 → 百炼 Fun‑ASR realtime（DashScope）**：按其 WebSocket 协议发 `run-task` 并推音频帧，使用 `max_sentence_silence` 等参数实现“服务端断句”。[4]
- **网关 → ISI/NLS 实时语音识别**：按其 WebSocket 协议推流，帧大小建议（1600/3200 bytes）对“稳定实时链路”很友好；如果你更看重“语音产品化能力/SDK 完整性”，通常优先选 ISI。[5]

这个方案“成熟”的原因是：所有高风险点（TLS、鉴权、协议细节、重试策略、日志）都发生在网关上；TAB5 端只要“持续稳定采音并传到局域网”，系统压力显著下降。

#### 方案 B（备选）：设备端直连百炼 Fun‑ASR realtime（DashScope WSS）
如果你坚持设备端直连，百炼 Fun‑ASR realtime 的协议本身是可实现的：`wss://dashscope.aliyuncs.com/api-ws/v1/inference/` → `run-task` → 二进制音频帧 → `finish-task`，并可通过参数控制断句/标点/语种提示。[4]

但“成熟性”的最大短板依旧是 **公网 WSS + mbedTLS 的资源峰值**。在不改总体架构的情况下，你能做的只是在边界上缓解：
- 调整 ESP‑IDF 的 mbedTLS 相关配置（动态缓冲、IN/OUT content len 等）以降低常驻内存。[2]
- 通过 ESP‑TLS 切换到不同 TLS 后端（如 WolfSSL），在某些场景可能更省内存或更适合你的配置组合（需要实际对比）。[3]

这条路的风险是：你可能“调到某一版固件稳定几小时”，但遇到网络重连/长时间运行又复发；因此我把它放在备选。

#### 方案 C（成熟但更重）：网关侧自建 ASR（如 FunASR / Whisper）
如果你希望“完全不依赖云”或希望把成本压到最低，网关侧可以自建 ASR：
- **FunASR（阿里开源）**提供离线/在线识别与相关模型生态。[6]
- 也有基于 FunASR 的实时方案与 WebSocket 协议参考（如 ASR‑2Pass 工程），提供 VAD/分段相关字段（如 `is_speaking` 等）。[7]

这条路“成熟”在工程可控性（你控制全栈），但代价是：需要 CPU/GPU 算力与维护成本；中文效果、延迟、噪声鲁棒性都需要你用真实语料压测才能定。

#### 方案 D（补充思路，不替代 ASR）：设备端做“小词表离线指令”
如果你希望“离线也能用”，ESP‑SR（Espressif 的语音识别库）更适合 **固定命令词识别**（不是大词表中文输入法/自由口述）。它可以作为 Voice 交互的兜底，例如“开始/取消/发送”等本地指令。[8]

### 3) 推荐的最短落地路线（你现在最省心的）
1) **先做方案 A**：TAB5 → 你局域网网关（无 TLS）推 16k/16bit/mono PCM。  
2) 网关先接 **百炼 Fun‑ASR realtime**（你已经有 API Key，最快闭环），断句先用 `max_sentence_silence`（静音阈值）配合服务端结果回调。[4]  
3) 等稳定后，再决定是否迁移到 ISI（更“语音产品线”）或自建 FunASR/Whisper。  
4) 如果你仍要“最终设备直连云端”，把它当“可选优化路线”，并始终保留网关作为 fallback。

## Areas of Consensus
- 公网 WSS（TLS + WS + 流式音频）对嵌入式设备资源峰值要求高；“能跑通”与“能长期稳定”是两回事，成熟方案优先用网关隔离风险。[4][5]
- ESP32 上 `-0x7F00` 这类 mbedTLS 错误通常是 TLS 内存分配失败，常见诱因是堆不足或碎片化导致的最大连续块不足。[1]
- ISI/NLS 文档对实时流式音频传输更“工程化”（推荐帧大小、格式等），适合追求稳定实时链路的场景。[5]

## Areas of Debate
- **百炼 Fun‑ASR vs ISI/NLS**：Fun‑ASR 更贴近百炼/模型平台生态（API Key、模型参数）；ISI 更像“语音产品线”（文档/SDK/语音场景更完善）。最终要看你账号权限、计费、并发以及在你的真实噪声环境下的效果。[4][5]
- **自建 ASR 的性价比**：自建可控但要运维与算力；云端省运维但要处理网络与成本，且设备端直连云端会把稳定性问题放大。

## Sources
[1] Espressif Techpedia — Mbed TLS common error troubleshooting（解释 `-0x7F00`/alloc failed 的典型原因与排查方向）. https://docs.espressif.com/projects/esp-techpedia/en/latest/esp-friends/advanced-development/protocol/mbedtls-troubleshooting.html

[2] ESP‑IDF v5.4 Kconfig reference（包含 `CONFIG_MBEDTLS_DYNAMIC_BUFFER` 等 mbedTLS 相关选项）. https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/kconfig.html

[3] ESP‑TLS component documentation（说明 ESP‑TLS 可在不同 TLS library 间切换，如 mbedTLS / WolfSSL）. https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-reference/protocols/esp_tls.html

[4] 阿里云百炼（Model Studio / DashScope）— Fun‑ASR realtime WebSocket API（`wss://dashscope.aliyuncs.com/api-ws/v1/inference/`，`run-task`/`finish-task`，流式 duplex，`max_sentence_silence`/`semantic_punctuation_enabled`/`language_hints` 等）. https://help.aliyun.com/zh/model-studio/fun-asr-realtime-websocket-api

[5] 阿里云智能语音交互（ISI / NLS）— WebSocket 协议与 SDK FAQ（音频格式、采样率、推荐每次发送 1600/3200 bytes 等）. https://help.aliyun.com/zh/isi/developer-reference/websocket ；https://help.aliyun.com/zh/isi/support/sdk-faq

[6] FunASR（Alibaba DAMO Academy）开源仓库（模型与工具生态）. https://github.com/alibaba-damo-academy/FunASR

[7] ASR‑2Pass（FunASR 相关实时识别工程）— WebSocket streaming 协议示例（包含 VAD/分段相关字段示例）. https://github.com/FunAudioLLM/ASR-2Pass/blob/main/websocket_protocol.md

[8] Espressif ESP‑SR（语音识别库，适合固定命令词识别等场景）. https://docs.espressif.com/projects/esp-sr/en/latest/esp32/README.html

## Gaps and Further Research
- **你账号侧的“真实成本/并发/配额”**：百炼与 ISI 的计费/并发策略需要以你账号实际控制台为准，建议做 10–30 分钟真实语料压测再最终选型。
- **网关协议定稿**：TAB5→网关用 WS 还是 HTTP chunked、是否需要局域网加密、断线重传策略等，需要结合你家里/办公室网络实际情况确定。
