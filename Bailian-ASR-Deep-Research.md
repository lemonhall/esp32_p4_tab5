# 阿里云百炼（Model Studio / DashScope）ASR 接入调研（TAB5 Voice 按键）

## Executive Summary
针对 TAB5 上的 “Voice” 按键语音输入场景，最合适的形态通常是 **实时语音识别（WebSocket）**：它天然支持低延迟分段回传，并可在服务端使用模型/服务内置的端点检测（VAD/静音检测相关参数）来自动结束一次“说话回合”。综合文档能力与可调参数，优先推荐从 `paraformer-realtime-v2` 开始；如更关注口音/多语言覆盖，可评估 `fun-asr-realtime` 作为备选。[1][2][3]

## Key Findings
- **实时 ASR 的官方入口与模型**：百炼（Model Studio / DashScope）提供实时语音识别 WebSocket 方式，官方推荐/示例覆盖 `paraformer-realtime-v2`、`fun-asr-realtime` 等模型。[1]
- **“服务器 VAD/端点检测”可以通过参数落地**：实时识别参数里提供 `semantic_punctuation_enabled`、`max_sentence_silence`、`multi_threshold_mode_enabled` 等，可用于“按下录音→持续送音频→服务端判断一句结束→回传最终文本”。[2][3]
- **API Key 获取与鉴权**：使用百炼 API Key（DashScope API Key）进行鉴权；实时 WebSocket 也支持通过 `wss://dashscope.aliyuncs.com/api-ws/v1/inference`（中国内地）接入。[4][2]
- **客户端（设备）可保持极简**：TAB5 侧只需录音与流式上传（不做 VAD），收到服务端最终文本后：Toast 显示几秒 + 同步发到 IRC 频道（你定义的业务流程）。[2]

## Detailed Analysis

### 1) 选型：用哪个 ASR 模型更合适？

#### 1.1 优先建议：`paraformer-realtime-v2`
理由（面向“语音输入→得到一句中文→发 IRC”）：
- 实时识别链路完整：官方有针对该模型的实时识别 SDK/示例与参数说明。[1][3]
- 参数可控：可在服务端通过 `max_sentence_silence` 等参数调节“停顿多久算一句结束”，更贴合你说的“服务器端做 VAD”。[3]

#### 1.2 备选：`fun-asr-realtime`
适用情况：
- 更看重多语言/方言覆盖或对噪声环境适配（以官方文档/示例为准）。[1][2]
- 需要更明确的“基于 VAD 的分段”配置：文档对 `semantic_punctuation_enabled=false`（启用基于 VAD 的断句）有更直接的说明。[2]

#### 1.3 何时用“录音文件识别”（非实时）？
如果你最终希望设备端“一次录 3–10 秒→上传文件→返回整段文本”，也可以用“录音文件识别（REST）”。但它更偏离你要的交互（等待更久、缺少实时分段反馈）；而且你提到“服务器端 VAD”，实时链路更自然。[5]

---

### 2) 服务端 VAD / 端点检测：怎么做成“说完自动停”

百炼实时识别的关键，是用服务端参数来决定分段策略与端点：

#### 2.1 用 “VAD 断句” 而不是 “语义断句”
官方文档说明：
- `semantic_punctuation_enabled=true`：基于语义断句（更“像标点/语义”）。[2]
- `semantic_punctuation_enabled=false`：基于 VAD 断句（更贴合“停顿就收句”）。[2]

你的需求是“嵌入式不做 VAD，让服务器做 VAD，并在一句结束后回传中文并结束录音状态”，因此建议默认从：
- `semantic_punctuation_enabled=false` 开始（基于 VAD 断句）。[2]

#### 2.2 调 `max_sentence_silence` 让体验更像“按一次说一句”
文档给出：
- `max_sentence_silence`：句中静音超过该阈值（毫秒）则断句并回传。[2][3]

可落地的经验值（需要你现场试听微调）：
- 800–1200ms：更“干脆”，适合短句指令。
- 1200–2000ms：更“宽容”，适合自然口语（避免频繁断句）。

#### 2.3 其他可能有用的参数
以下参数在文档中出现，可作为后续调优开关：
- `multi_threshold_mode_enabled`：多阈值模式（更适合自由说，避免单一阈值过敏/过钝）。[2][3]
- `punctuation_prediction_enabled`：标点预测（输出更可读；IRC 里是否需要可按偏好）。[3]
- `inverse_text_normalization_enabled`：ITN（数字/日期等格式化）。[3]

---

### 3) 调用方式与文档入口（你服务器实现的“最短路径”）

#### 3.1 WebSocket 推理入口（中国内地）
实时识别基于 WebSocket 推理通道：
- `wss://dashscope.aliyuncs.com/api-ws/v1/inference`（北京地域）[2]

> 备注：同一套 WebSocket 推理入口也用于其它实时推理能力；因此文档会以“WS 推理”+“实时 ASR 参数”组合呈现。[2][1]

#### 3.2 SDK/示例：建议先用官方示例跑通
官方 SDK 示例里展示了：
- 指定模型（如 `paraformer-realtime-v2`）
- 指定音频格式与采样率（示例常见为 PCM / 16k）
- 在回调里判断 `sentence_end` 等字段，用于识别“本句结束”（非常贴合你要的“结束录音状态”）。[3]

#### 3.3 API Key 获取
API Key 的获取与管理按百炼官方文档流程进行（控制台创建/查看）。[4]

---

### 4) 结合 TAB5 的 UI 交互建议（与“服务端 VAD”对齐）

建议 UI 状态机（设备侧只管录音与展示）：
1. **Idle**：Voice 按键为可点击状态。
2. **Recording**（点击 Voice 进入）：按钮变为“红点/波形/计时”；设备开始录音并把音频流推给你的服务器（不做 VAD）。
3. **Finalizing**（服务端检测一句结束）：设备停止录音；等待最终文本。
4. **Toast + Send**：收到文本后，弹出 Toast（持续 3–5 秒）显示中文识别结果；同时把该文本发送到 IRC 频道；随后回到 Idle。

该流程的关键点是：服务端通过百炼实时识别的回调/字段判断“句结束”，再把最终文本一次性回传给设备。[3]

## Areas of Consensus
官方资料总体一致地建议：实时语音识别使用 WebSocket 推理通道，适合交互式语音输入；并且提供了用于分句/端点的参数与回调字段，可由服务端完成“VAD/端点检测”式的自动收句。[1][2][3]

## Areas of Debate
- **VAD 断句 vs 语义断句**：`semantic_punctuation_enabled` 的取值会影响断句行为；你要“像对讲机一样一句一句说”更偏 VAD 断句，但是否更符合期望仍需现场调参。[2]
- **模型选择的“主观最优”**：`paraformer-realtime-v2` 与 `fun-asr-realtime` 在不同噪声/口音/语速下表现可能不同；建议用你的实际语料做 A/B 对比再定。[1]

## Sources
[1] Alibaba Cloud Model Studio / DashScope 文档：实时语音识别介绍与模型推荐（官方文档，可信度高）。https://help.aliyun.com/zh/model-studio/realtime-speech-recognition

[2] Alibaba Cloud Model Studio / DashScope 文档：FunASR 实时语音识别 WebSocket API（参数含 `semantic_punctuation_enabled`、`max_sentence_silence`、WS endpoint 等；官方文档，可信度高）。https://help.aliyun.com/zh/model-studio/developer-reference/websocket-api-for-funasr-realtime-speech-recognition

[3] Alibaba Cloud Model Studio / DashScope 文档：Paraformer 实时语音识别 Java SDK 示例（包含 `max_sentence_silence` 等参数与 `sentence_end` 回调字段；官方文档，可信度高）。https://help.aliyun.com/zh/model-studio/developer-reference/java-sdk-api-for-paraformer-realtime-speech-recognition

[4] Alibaba Cloud Model Studio / DashScope 文档：获取 API Key（官方文档，可信度高）。https://help.aliyun.com/zh/model-studio/get-api-key

[5] Alibaba Cloud Model Studio / DashScope 文档：录音文件识别 REST API（官方文档，可信度高）。https://help.aliyun.com/zh/model-studio/developer-reference/recorded-speech-recognition-rest-api

[6] Alibaba Cloud DashScope Java SDK（GitHub，示例与 SDK 版本信息可用于落地验证；可信度中高）。https://github.com/alibabacloud-ai/dashscope-sdk-java

## Gaps and Further Research
- **计费/配额与限流**：需要结合你的百炼账号与项目配额核对并压测（官方可能分模型/并发计费）。[1]
- **更细的“结束一次会话”协议细节**：WS 推理协议的消息序列（start/audio/end）在不同 SDK/直连实现里会影响边界行为；如你计划不使用官方 SDK，需要进一步对照“WS 推理通道协议”做实现核验。[2][6]

