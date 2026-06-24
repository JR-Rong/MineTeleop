# ADR 0001: 传输与媒体栈选择

## Status

Accepted for v1 planning.

## Context

系统需要在车端和驾驶端不处于同一局域网的情况下传输 4 路实时视频，并回传控制命令。网络是 5G，要求低延迟、低流量、可穿透 NAT，并支持云端中继兜底。

候选方案：

- WebRTC + STUN/TURN。
- SRT/RTP + 自定义控制协议。
- RTSP/HTTP 低延迟变体。

## Decision

首版采用 WebRTC 作为实时视频和控制通道主线，云端提供 HTTPS/WebSocket 信令和 STUN/TURN 兜底。

车端和驾驶端媒体实现优先评估 GStreamer/WebRTC。FFmpeg 用于编码能力验证和部分工具链，但完整实时 WebRTC pipeline 需单独验证。

## Rationale

WebRTC 更适合公网/NAT 场景：

- 支持 ICE/STUN/TURN。
- 支持 UDP 低延迟传输。
- 支持拥塞控制。
- 支持音视频和 DataChannel。
- DTLS/SRTP 安全机制成熟。

RTSP/HTTP 更容易实现，但通常不适合作为 50-150 ms 遥操主链路。

SRT/RTP 可控性强，但信令、控制、安全、NAT 兜底和多端兼容需要自建更多组件。

## Consequences

正面：

- 公网连接成功率更高。
- 可用 TURN 兜底。
- 驾驶端和车端可复用成熟媒体组件。

代价：

- WebRTC 调试复杂。
- ICE/TURN/网络路径需要专门监控。
- 如果 GStreamer 缺少硬编插件，需要补齐媒体运行环境。

## Follow-up

- 验证 GStreamer `webrtcbin` 与 Intel 硬编插件组合。
- 如果 GStreamer 硬编不可用，评估 WebRTC native + VAAPI 或 FFmpeg 编码后接入 RTP/WebRTC 的可行性。
- 建立弱网测试基线。

