# ADR 0001: 传输与媒体栈选择

## Status

Accepted and implemented for the media path.

## Context

系统需要在车端和驾驶端不处于同一局域网的情况下传输 4 路实时视频，并回传控制命令。网络是 5G，要求低延迟、低流量、可穿透 NAT，并支持云端中继兜底。

候选方案：

- WebRTC + STUN/TURN。
- SRT/RTP + 自定义控制协议。
- RTSP/HTTP 低延迟变体。

## Decision

实时视频采用 WebRTC/DTLS/SRTP，云端提供已认证的信令消息队列和 STUN/TURN 兜底。

车端使用进程内 GStreamer `webrtcbin`，浏览器使用原生 WebRTC 连续解码；不再
通过逐帧 JSON/Base64/HTTP 传输，也不启动 FFmpeg 录制进程。

普通控制命令使用独立的已认证原生 WSS：控制端以 20 Hz 发送完整状态，signaling
只保留容量 1、TTL 150 ms 的 latest-only mailbox，车端通过 control-only WSS 接收并
按 `seq` 去重。WebRTC DataChannel 已用于 session profile、VCU 握手和状态，但不承载
普通控制命令。

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

- 验证 GStreamer `webrtcbin` 与 NVENC/Intel VAAPI 的 H.265、H.264 组合。
- 验证实际浏览器 WebRTC H.265/H.264 SDP 协商和硬件解码。
- 注入 NVIDIA 故障并验收 Intel 回退。
- 建立弱网测试基线。
