# 云端服务设计

## 职责

云端服务负责连接协调、鉴权、会话管理、审计和上传目标管理。它不应该承载车辆安全控制的最终责任。

云端组件：

- Auth Service：用户和设备鉴权。
- Signaling Service：WebRTC 信令。
- Session Service：会话和控制权。
- TURN Service：NAT 穿透兜底。
- Upload API：录像上传凭证或上传登记。
- Audit Log：审计日志。

## 部署建议

实时服务与普通业务隔离。

建议：

- 固定 IP 云服务器保留为入口和管理节点。
- 新增支持 UDP 的轻量节点作为 STUN/TURN。
- TURN 节点尽量靠近车辆和驾驶端网络路径。
- 如果现有云服务器负载较高，不建议让 4 路视频长期全部经它中继。

## 鉴权

### 车端设备

推荐：

- 每台车一个设备 ID。
- 每台车一组设备证书或长期凭据。
- 车端启动后向云端注册在线状态。
- 云端校验设备身份后允许建立会话。

### 驾驶端用户

推荐：

- 用户登录获取短期令牌。
- 令牌用于信令和会话请求。
- 控制权只授予一个用户。

## 会话模型

首版一车一驾驶员。

会话状态：

- `IDLE`：车辆在线但无人控制。
- `REQUESTED`：驾驶端请求连接。
- `ACTIVE`：会话已建立，控制权有效。
- `ENDING`：会话结束中。
- `ENDED`：会话结束。
- `FAILED`：会话失败。

控制权规则：

- 同一时刻一车只有一个控制者。
- 控制权发放需要云端记录。
- 车端也需要校验会话 ID 和令牌，不能只信驾驶端。

## 信令

信令服务建议使用 WebSocket。

消息类型：

- `vehicle_online`
- `vehicle_offline`
- `driver_login`
- `session_request`
- `session_accept`
- `session_reject`
- `webrtc_offer`
- `webrtc_answer`
- `ice_candidate`
- `session_end`
- `control_authority_revoked`

信令只负责建立连接和会话状态，不转发每条控制命令。

## TURN

TURN 用于 P2P 失败或网络不稳定时兜底。

要求：

- 支持 UDP。
- 配置长期凭据或临时凭据。
- 日志记录中继流量。
- 对不同车辆或会话做带宽统计。

注意：

- TURN 会增加延迟和云端流量成本。
- TURN 节点不要和高负载业务混用。

## 录像上传

默认设计为对象存储/S3 兼容。

两种上传模式：

1. 车端从云端获取预签名上传 URL，直接上传对象存储。
2. 车端上传到云端 Upload API，由后端转存对象存储。

推荐第一种：

- 云服务器不承载大文件转发。
- 扩展性更好。
- 可以按会话/车辆/日期组织对象路径。

对象路径建议：

```text
vehicles/{vehicle_id}/sessions/{session_id}/cameras/{camera_id}/{start_ts}.mp4
vehicles/{vehicle_id}/sessions/{session_id}/metadata/{segment_id}.json
```

## 审计日志

云端至少记录：

- 用户登录。
- 车辆上线/下线。
- 会话创建/结束。
- 控制权授予/回收。
- 急停。
- 控制链路超时。
- TURN 中继启用。
- 上传成功/失败。

