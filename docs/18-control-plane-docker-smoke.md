# 原生 C++ 控制面 Docker smoke

`scripts/test/run_control_plane_docker_smoke.sh` 是本地开发检查，不是浏览器、真实
WebRTC/DataChannel 或现场车辆验收。它构建 Ubuntu 22.04 原生 C++ 镜像，启动：

- loopback 开发信令服务器；
- 本地驾驶页面服务器；
- 一次性 `control-smoke` 客户端。

smoke 验证以下边界：

1. 车辆凭据可注册在线连接并取得 generation。
2. 驾驶员可登录、创建唯一活动会话并取得短期控制权。
3. `/api/control-intent` 只更新当前 session/generation 的 latest-only 浏览器输入意图，
   返回 `transport=native_signaling_websocket`；旧 `/api/control` 不再准备命令。
4. 原生控制端拥有 20 Hz 命令生成时钟、协议元数据、命令 `seq` 和短期 control token，
   普通 `control_command` 进入服务端容量 1、TTL 150 ms 的专用 mailbox。
5. 命令的 vehicle、driver、session、control token、`intent_seq` 和 `intent_fresh` 与
   当前会话/意图一致；控制端 `/api/status.native_control` 暴露发送与 ACK 进度。
6. `media_capabilities` 仍通过会话隔离的 WebRTC 信令队列转发，与控制 mailbox 分离。
7. 控制端 `/api/status` 保持已连接状态。

运行：

```bash
scripts/test/run_control_plane_docker_smoke.sh
```

通过输出包含：

```json
{
  "event": "native_control_plane_smoke",
  "runtime": "cpp",
  "passed": true,
  "control_transport": "native_signaling_websocket",
  "control_intent_accepted": true,
  "control_command_delivered": true
}
```

日志和结果写入 `.local/control-plane-smoke/<UTC-like timestamp>/`。开发凭据仅用于
loopback smoke；不得用于公网。

## 浏览器边界

`scripts/test/run_control_plane_browser_smoke.sh` 当前只是上述 HTTP/control-plane smoke 的
包装器。它没有启动真实浏览器 peer 或完整 vehicle runtime，不能证明视频轨、
profile/VCU/status DataChannel、持续 20 Hz 时序、TURN relay、独立车端 watchdog 或
链路关闭安全停车。

真实浏览器验收必须至少包含：

- 浏览器收到 GStreamer `webrtcbin` 建立的 profile/VCU/status DataChannel，且
  `ordered=false`、`maxRetransmits=0`、协议名正确；普通控制命令不得走该通道；
- 浏览器仅更新 `/api/control-intent`，即使页面隐藏/冻结，原生进程仍按 20 Hz 经
  专用 WSS 发包；车端通过独立 control-only WSS 接收并由独立 50 ms watchdog 检查；
- 重复、乱序、过期、stale 非零、错会话和错 token 被拒绝；只有满足
  `CONTROL_ACTIVE`、profile/握手、原生包间隔和最后新鲜挡位门禁的精确零值 stale
  heartbeat 可以维持当前控制，不能建立或恢复控制；
- 关闭页面结束会话、断开 DataChannel、停止原生控制进程和断开控制 WSS 时，车端
  分别按权限门禁或 watchdog 本地安全停车；
- 两路真实视频同时显示，并记录直连/STUN/TURN、FPS、丢包、RTT 和端到端时延。

这些结果应作为独立验收 artifact 保存，不能用本 Docker smoke 代替。
