# 原生 C++ 控制面 Docker smoke

`scripts/run_control_plane_docker_smoke.sh` 是本地开发检查，不是浏览器、WebRTC
DataChannel 或现场车辆验收。它构建 Ubuntu 22.04 原生 C++ 镜像，启动：

- loopback 开发信令服务器；
- 本地驾驶页面服务器；
- 一次性 `control-smoke` 客户端。

smoke 验证以下边界：

1. 车辆凭据可注册在线连接并取得 generation。
2. 驾驶员可登录、创建唯一活动会话并取得短期控制权。
3. `/api/control` 只生成将由浏览器发送的协议 v1 完整命令，返回
   `transport=webrtc_data_channel`；它不把命令放入服务器信令队列。
4. 命令的 vehicle、driver、session 和短期 control token 与车端看到的会话一致。
5. `media_capabilities` 仍通过会话隔离的 WebRTC 信令队列转发。
6. 控制端 `/api/status` 保持已连接状态。

运行：

```bash
scripts/run_control_plane_docker_smoke.sh
```

通过输出包含：

```json
{
  "event": "native_control_plane_smoke",
  "runtime": "cpp",
  "passed": true,
  "control_transport": "webrtc_data_channel",
  "control_command_prepared": true
}
```

日志和结果写入 `.local/control-plane-smoke/<UTC-like timestamp>/`。开发凭据仅用于
loopback smoke；不得用于公网。

## 浏览器边界

`scripts/run_control_plane_browser_smoke.sh` 当前只是上述 HTTP/control-plane smoke 的
包装器。它没有启动真实浏览器 peer，也不能证明视频轨、SCTP DataChannel、20 Hz、
TURN relay 或链路关闭安全停车。

真实浏览器验收必须至少包含：

- 浏览器收到 GStreamer `webrtcbin` 建立的 `control` DataChannel；
- `ordered=false`、`maxRetransmits=0`、协议名正确；
- 20 Hz 完整状态到达车端，重复、乱序、过期、错会话和错 token 被拒绝；
- 关闭页面、断开 DataChannel 和断网时车端本地全停；
- 两路真实视频同时显示，并记录直连/STUN/TURN、FPS、丢包、RTT 和端到端时延。

这些结果应作为独立验收 artifact 保存，不能用本 Docker smoke 代替。
