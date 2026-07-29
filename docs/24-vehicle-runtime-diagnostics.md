# 车端摄像头、WebRTC 与 VCU/CAN 故障日志全集

本文把“全集”限定为当前仓库实现中，从有效车端 YAML 已加载后能够到达的显式失败
出口、超时、拒绝、降级和安全停车路径。硬件尚未被软件观察到的故障（例如摄像头
完全未枚举、CAN 线物理断路但内核尚未报错）会通过相邻的超时或缺失反馈事件呈现，
不能凭软件日志区分其全部物理根因。

所有新增诊断事件都使用以下公共字段：

- `event`：便于直接 grep 的稳定事件名；
- `issue_code`：一个具体故障类别的稳定机器码；
- `stage`：失败发生的生命周期阶段；
- `error`：本次系统调用、驱动或协议返回的原始错误；
- `operator_action`：下一步排查动作；
- `retryable`：运行时是否可以安全重试；
- `vehicle_id`、`driver_id`、`session_id`：会话关联字段（可用时）；
- `safety_action`：故障后的控制安全动作（涉及控制时）。

摄像头/WebRTC 诊断写入车端 runtime 的 stdout/stderr。VCU 的启动可见性事件也写入
stderr；协议细节与高频 CAN 证据写入
`MINE_TELEOP_VCU_LOG_PATH`（默认
`/var/log/mine-teleop/vcu-can.jsonl`）。

## 配置加载与子进程边界

配置尚未成功加载时，无法可靠判断错误属于哪一路 camera/VCU，因此入口统一输出
`mine_teleop_error`，包含 `issue_code=runtime_entry_failed`、`stage=process_entry` 和
原始 YAML/字段/路径错误。先运行 `config-check` 与 `vehicle-agent --preflight`。
需要注意：preflight 只证明 V4L2 路径存在，不能证明该节点具有
`VIDEO_CAPTURE + STREAMING` capability；后者由下面的
`camera_node_not_capture_capable` 精确报告。

`vehicle-runtime` 的 media 子进程若因未处理异常退出，会输出
`vehicle_runtime_child_error`、`service=media`、
`issue_code=vehicle_runtime_child_failed` 与 `safety_action=local_full_stop`。
它之前应已有更具体的 camera/WebRTC/VCU 事件；该事件是进程边界兜底，不取代具体
根因事件。

## 摄像头采集故障全集

所有行都即时输出 `event=vehicle_camera_failed`；`camera_id`、`device`、
`source_kind`、配置分辨率/FPS 以及 captured/pushed/encoded/dropped 计数用于直接
定位是哪一路摄像头。

| `issue_code` | 触发条件/根因边界 | `stage` | 首要排查动作 |
| --- | --- | --- | --- |
| `camera_config_invalid` | ID、分辨率、FPS 或 native acquisition profile 非法 | `camera_config` | 检查 camera 与 realtime profile |
| `camera_source_type_invalid` | 既不是 testsrc/V4L2，也不是支持的 SDK selector | `camera_config` | 修正 `device` selector |
| `camera_bridge_pipe_failed` | vendor bridge stdout pipe 创建失败 | `vendor_bridge_start` | 检查 fd 上限和系统资源 |
| `camera_bridge_fork_failed` | vendor bridge 进程 fork 失败 | `vendor_bridge_start` | 检查进程上限和内存 |
| `camera_bridge_exited` | bridge 未输出完整 JPEG 就退出 | `vendor_bridge_capture` | 单独运行 bridge 并看 stderr |
| `camera_bridge_read_failed` | 读取 bridge stdout 失败 | `vendor_bridge_capture` | 检查 bridge/SDK 与进程状态 |
| `camera_frame_too_large` | 单帧 MJPEG 超过 16 MiB | `camera_capture` | 检查输出格式和帧边界 |
| `camera_frame_timeout` | V4L2 或 vendor bridge 在超时内无帧 | `camera_capture` | 检查供电、链路、节点、占用与 FPS |
| `camera_poll_failed` | poll 系统调用失败 | `camera_capture` | 查内核日志并重连设备 |
| `camera_open_failed` | V4L2 open 失败（不存在、权限、busy 等） | `v4l2_open` | 检查路径、权限和占用 |
| `camera_querycap_failed` | `VIDIOC_QUERYCAP` 失败 | `v4l2_capabilities` | 确认路径确为 V4L2 节点 |
| `camera_node_not_capture_capable` | 节点没有 capture+streaming capability | `v4l2_capabilities` | 选择 `v4l2-ctl` 显示的 capture 节点 |
| `camera_mjpeg_format_rejected` | 驱动拒绝 MJPEG 宽高 | `v4l2_format` | 使用驱动公布的 MJPEG mode |
| `camera_native_mjpeg_unavailable` | 实际协商结果不是 MJPEG | `v4l2_format` | 换 native MJPEG mode 或实现转换 |
| `camera_fps_rejected` | 驱动拒绝目标 FPS | `v4l2_frame_rate` | 使用该分辨率公布的 FPS |
| `camera_mmap_buffers_unavailable` | `REQBUFS` 失败或少于两个 buffer | `v4l2_buffers` | 查驱动 streaming 支持与内存 |
| `camera_query_buffer_failed` | `QUERYBUF` 失败 | `v4l2_buffers` | 查驱动/USB 链路 |
| `camera_mmap_failed` | 用户态 mmap 失败 | `v4l2_buffers` | 查内存压力与驱动 |
| `camera_queue_buffer_failed` | 初始化或运行中 `QBUF` 失败 | `v4l2_stream` | 查驱动/USB 链路并重启采集 |
| `camera_stream_on_failed` | `STREAMON` 失败 | `v4l2_stream` | 查占用、USB 带宽与 mode |
| `camera_dequeue_buffer_failed` | `DQBUF` 失败 | `v4l2_capture` | 查驱动/USB 错误 |
| `camera_invalid_capture_buffer` | 驱动返回越界 index/bytesused | `v4l2_capture` | 按驱动故障处理并查 kernel log |
| `camera_invalid_mjpeg_frame` | JPEG SOI/EOI 校验失败 | `camera_decode_boundary` | 查相机/bridge 帧边界和数据完整性 |
| `camera_test_jpeg_encode_failed` | testsrc native JPEG 编码失败 | `test_source_encode` | 查 JPEG runtime 与目标尺寸 |
| `camera_gstreamer_buffer_allocation_failed` | GstBuffer 分配失败 | `gstreamer_push` | 查内存压力 |
| `camera_appsrc_push_failed` | appsrc 拒绝非 flushing/EOS 帧 | `gstreamer_push` | 结合 GStreamer bus error 排查下游 |
| `camera_capture_failed` | 未归类的 camera source 异常兜底 | `camera_capture` | 使用原始 `error` 定位 |

成功拿到每路第一帧时输出 `vehicle_camera_first_frame`。因此，“没有
`vehicle_camera_first_frame`，随后出现 `vehicle_camera_failed`”可以直接判定为采集
侧问题；不会再等到最终 `vehicle_media_webrtc_summary` 才能看到。

## 编码、WebRTC、DataChannel 与信令故障全集

| `event` / `issue_code` | 触发条件 | 关键附加字段 | 频率 |
| --- | --- | --- | --- |
| `vehicle_media_time_sync_failed` / `media_initial_time_sync_failed` | 初始时间同步请求失败 | `error` | 每次 session 尝试一次 |
| `vehicle_media_time_sync_failed` / `media_time_sync_uncertainty_exceeded` | 初始不确定度超过安全阈值 | `uncertainty_ms`, `limit_ms` | 即时 |
| `vehicle_media_signaling_failed` / `vehicle_signaling_registration_failed` | vehicle online 注册失败 | `error` | 每次重连一次 |
| `vehicle_media_signaling_failed` / `vehicle_session_discovery_failed` | session 查询失败/connection generation 失效 | `error` | 每次失败一次 |
| `vehicle_media_session_wait_failed` / `active_driver_session_timeout` | 非 service 模式 5 秒无 active session | `error` | 即时 |
| `vehicle_media_signaling_failed` / `ice_server_fetch_failed` | 获取 STUN/TURN 配置失败 | `error` | 每次 session 一次 |
| `vehicle_media_negotiation_failed` / `media_codec_negotiation_failed` | 浏览器能力缺失或 H264/H265 不相交 | `error` | 每次 session 一次 |
| `vehicle_media_negotiation_failed` / `video_encoder_candidates_empty` | 无 encoder candidate | `error` | 即时 |
| `vehicle_media_pipeline_failed` / `encoder_factory_unavailable` | NVENC/VAAPI factory 不存在 | `backend`, `codec` | 每个 candidate 一次 |
| `vehicle_media_pipeline_failed` / `gstreamer_pipeline_build_failed` | pipeline parse/build 失败 | `error`, backend/codec | 每个 candidate 一次 |
| `vehicle_media_pipeline_failed` / `gstreamer_webrtcbin_missing` | pipeline 中找不到 webrtcbin | backend/codec | 每个 candidate 一次 |
| `vehicle_media_pipeline_failed` / `webrtc_ice_server_config_failed` | TURN 凭据/URI/add-turn-server 失败 | `error` | 每个 candidate 一次 |
| `vehicle_media_pipeline_failed` / `gstreamer_ready_state_failed` | pipeline 不能进入 READY | backend/codec | 每个 candidate 一次 |
| `vehicle_vcu_adapter_start_failed` / `vcu_adapter_start_failed` | 动态库、符号、CAN、日志路径或 adapter open 失败 | adapter/interface/bitrate/tx queue/library | 每个 candidate 一次 |
| `vehicle_media_pipeline_failed` / `control_data_channel_create_failed` | webrtcbin 未创建 SCTP DataChannel | `error` | 每个 candidate 一次 |
| `vehicle_media_pipeline_failed` / `gstreamer_camera_lane_incomplete` | 某路 appsrc/encoder 元素缺失 | `camera_id`, `device` | 即时 |
| `vehicle_media_pipeline_failed` / `gstreamer_playing_state_failed` | pipeline 不能进入 PLAYING | backend/codec | 每个 candidate 一次 |
| `vehicle_media_pipeline_failed` / `webrtc_offer_promise_failed` | create-offer promise 失败 | backend/codec | 即时 |
| `vehicle_media_pipeline_failed` / `webrtc_offer_missing` | promise 没有 offer | backend/codec | 即时 |
| `vehicle_media_pipeline_failed` / `webrtc_answer_sdp_invalid` | 控制端 answer SDP 无法解析 | `error` | 即时 |
| `vehicle_media_pipeline_failed` / `webrtc_answer_missing` | 一次有限运行结束仍无 answer | ICE 里程碑事件 | 每个 candidate 一次 |
| `vehicle_media_pipeline_failed` / `browser_codec_fallback_requested` | 浏览器要求 H265 降级 H264 | reason/backend/codec | 即时并切换 candidate |
| `vehicle_media_pipeline_failed` / `gstreamer_bus_error` | 运行中 GStreamer error message | 完整 error+debug | 每个 candidate 首错一次 |
| `vehicle_media_pipeline_failed` / `video_frames_not_encoded` | 有 pipeline 但 encoded 总数为 0 | lane counters | 每个 candidate 一次 |
| `vehicle_control_data_channel_not_ready` / `control_data_channel_open_timeout` | answer 后 5 秒 DataChannel 未 open | local/remote ICE counts | 每个 candidate 一次 |
| `vehicle_control_data_channel_error` | DataChannel 回调报错 | `error` | 每次错误 |
| `vehicle_control_data_channel_closed` | 已打开的 channel 关闭 | 命令计数、最后接收时间 | 每次状态变化 |
| `vehicle_media_pipeline_failed` / `media_runtime_time_sync_failed` | session 中刷新时间同步失败 | `error` | 每个 candidate 首错一次 |
| `vehicle_media_pipeline_failed` / `media_runtime_time_sync_uncertainty_exceeded` | 刷新不确定度越界 | limit | 每个 candidate 首错一次 |
| `vehicle_media_signaling_failed` / `session_signaling_exchange_failed` | offer/answer/ICE 消息收发失败 | `error` | 每次 session 失败 |
| `vehicle_recording_sidecar_failed` / `recording_sidecar_write_failed` | 录像 sidecar 创建/rename 失败 | `error` | 每次 finalize 失败 |
| `vehicle_camera_performance_failed` / `camera_encoded_fps_below_minimum` | 任一路编码 FPS 低于阈值 | camera/FPS/frame counts | 每次 summary |
| `vehicle_camera_performance_warning` / `camera_capture_to_encode_latency_high` | capture→encode 峰值超过预算 | camera/latency/budget | 每次 summary |

正向里程碑为 `vehicle_webrtc_offer_created`、
`vehicle_webrtc_answer_applied`、`vehicle_webrtc_ice_candidate_discovered`、
`vehicle_webrtc_remote_ice_candidate_received` 和
`vehicle_control_data_channel_open`。页面出现“等待视频轨道”或“DataChannel 尚未
就绪”时，应从最后一个已出现的里程碑之后开始排查。

## VCU/CAN 故障全集

### 启动、动态库与日志

| `event` / `issue_code` | 触发条件 | 去哪里看 |
| --- | --- | --- |
| `vehicle_vcu_adapter_start_failed` / `vcu_adapter_start_failed` | dlopen、ABI symbol、接口未 UP、波特率不匹配、发送队列设置或 bridge open 任一步失败 | runtime stderr |
| `vehicle_vcu_start_failed` / `vcu_can_interface_invalid` | CAN interface 为空 | runtime stderr |
| `vehicle_vcu_start_failed` / `vcu_bridge_already_open` | 重复 open | runtime stderr |
| `vehicle_vcu_start_failed` / `chassis_control_initialize_failed` | `Initialize` 返回 false | runtime stderr |
| `vehicle_vcu_start_failed` / `chassis_control_emergency_seed_failed` | emergency seed 被拒 | runtime stderr |
| `vehicle_vcu_start_failed` / `chassis_control_initial_seed_failed` | neutral seed 被拒 | runtime stderr |
| `vehicle_vcu_start_failed` / `vcu_initial_command_invalid` | ChassisControl 输出越过 JYR010 command limits | runtime stderr |
| `vehicle_vcu_log_open_failed` / `vcu_log_directory_create_failed` | 无法创建日志目录 | runtime stderr |
| `vehicle_vcu_log_open_failed` / `vcu_log_file_open_failed` | 无法打开日志文件 | runtime stderr |
| `vehicle_vcu_log_write_failed` / `vcu_log_write_failed` | 运行中写失败 | runtime stderr |
| `vehicle_vcu_log_write_failed` / `vcu_log_flush_failed` | flush 失败 | runtime stderr |
| `vehicle_vcu_log_write_failed` / `vcu_log_rotation_failed` | 轮转 remove/rename 失败 | runtime stderr |
| `vehicle_vcu_log_write_failed` / `vcu_log_rotation_reopen_failed` | 轮转后无法 reopen | runtime stderr |
| `socket_open_failed` / `socketcan_open_failed` | interface 名、socket、ifindex、bind、fcntl 任一步失败 | VCU JSONL，含精确 `stage/errno/error` |

`vehicle_vcu_log_ready` 明确打印实际 `log_path`；VCU 日志自身的 `session_start` 和
`socket_opened` 证明日志与 SocketCAN 都已启动。

### 收发、协议与实时性

| VCU JSONL `name` / `issue_code` | 触发条件 | 证据与动作 |
| --- | --- | --- |
| `can_receive_failed` / `socketcan_receive_failed` | read 或短帧失败 | `stage/errno/error/interface`；本地全停 |
| `can_error_or_rtr_frame_ignored` / `can_error_or_rtr_frame_received` | 收到 CAN error/RTR frame | 每秒聚合；查 bus-off/error counter |
| `can_rx_ignored_summary` / `can_rx_unrecognized_or_invalid` | JYR010 decoder 不识别或 DLC 不合法 | 每秒聚合 count/last ID |
| `feedback_timeout` / `vcu_critical_feedback_timeout` | Ready 后 29 个关键 ID 任一超过 500 ms | `stale_ids` 与逐 ID `age_ms`；锁存故障并全停 |
| `can_send_failed` / `socketcan_send_failed` | 连续 3 个 TX 周期有发送失败 | errno、失败 ID；锁存故障并全停 |
| `tx_deadline_miss` / `vcu_tx_deadline_missed` | 20 ms 调度 deadline 落后 | 每秒至多一次，含 `lag_ms` |
| `io_thread_exception` / `vcu_io_thread_exception` | I/O 线程标准异常 | 原始 exception；本地全停 |
| `io_thread_exception` / `vcu_io_thread_unknown_exception` | I/O 线程非标准异常 | 本地全停 |

原始证据不做抽样：每个识别的 RX frame 使用 `kind=can_rx`，每个 20 ms 的完整 16
帧 TX 使用 `kind=can_tx_batch`。结论事件与原始帧可以按时间戳关联。

### 握手、控制与安全停车

| `event`/VCU JSONL `name` / `issue_code` | 触发条件 | 安全结果 |
| --- | --- | --- |
| `parallel_handshake_rejected` / `vcu_handshake_runtime_unavailable` | bridge 停止或已有 I/O fault | 保持停车 |
| `parallel_handshake_rejected` / `vcu_handshake_gate_rejected` | P/零速/EPB/manual state/新鲜度任一不满足 | 日志记录全部 gate 值 |
| `parallel_handshake_requested` / `vcu_handshake_requested` | 请求被接受 | 仍停车直至 Ready |
| `vehicle_vcu_handshake_state_changed` / `vcu_handshake_state_changed` | browser 可见握手状态变化 | stdout 只在状态变化时输出 |
| `control_apply_rejected` / `vcu_control_runtime_unavailable` | runtime/I/O fault 阻止控制 | 本地全停 |
| `control_apply_rejected` / `vcu_control_command_invalid` | command 越界或状态不允许 | 本地全停 |
| `bridge_api_operation_failed` / `chassis_control_update_failed` | ChassisControl 拒绝新的 vehicle state | 本地全停 |
| `bridge_api_operation_failed` / `vcu_apply_arguments_invalid` | ABI gear/pointer/count 非法 | 本地全停 |
| `vehicle_control_command_rejected` / `vcu_feedback_blocks_control` | feedback missing/poll failed 阻止 DataChannel 命令 | 本地全停；结合 VCU JSONL |
| `vehicle_vcu_handshake_command_failed` / `vcu_handshake_command_failed` | handshake ABI 调用抛错 | 本地全停 |
| `vehicle_vcu_runtime_failed` / `vcu_runtime_operation_failed` | tick/telemetry/safe-stop 路径抛错 | media attempt 停止并重试 |
| `emergency_stop` / `vcu_emergency_stop_applied` | 软件急停已下发 | 本地全停 |
| `emergency_stop_rejected` / `vcu_emergency_stop_runtime_unavailable` | bridge 已停止，软件急停无法下发 | 必须使用独立硬件安全路径 |
| `parallel_handshake_disconnect_requested` / `vcu_disarm_requested` | 主动断开 | 零扭矩、N、EPB、清握手 |
| `disarm_complete` / `vcu_disarm_complete` | 反向握手完成 | 全停已确认 |
| `disarm_timeout` / `vcu_disarm_timeout` | 15 秒内未完成反向握手 | 保持隔离并使用硬件安全路径 |
| `vehicle_vcu_safe_stop_failed` / `vcu_safe_stop_or_close_failed` | adapter close/safe stop 抛错 | 保持隔离，禁止仅凭软件判断安全 |

控制/握手操作拒绝按“原因变化或每秒一次”限频；DataChannel 普通控制拒绝按“原因
变化或每 5 秒一次”限频。安全状态变化、I/O fault、feedback timeout 与
disarm 结果立即 flush。

## 推荐排查命令

车端 runtime 只看失败和关键里程碑：

```bash
./bin/mine-teleop-run 2>&1 |
  jq -c 'select(
    (.severity == "error") or
    (.event | test("first_frame|offer_created|answer_applied|ice_candidate|data_channel_open|vcu_adapter_ready"))
  )'
```

定位某一路摄像头：

```bash
./bin/mine-teleop-run 2>&1 |
  jq -c 'select(.camera_id == "front_uvc2" or .issue_code == "camera_node_not_capture_capable")'
```

VCU 结论事件：

```bash
jq -c 'select(.kind == "event" and (.issue_code // "" != ""))' \
  /var/log/mine-teleop/vcu-can.jsonl*
```

VCU timeout 前后的原始帧：

```bash
jq -c 'select(
  .kind == "can_rx" or
  .kind == "can_tx_batch" or
  .name == "feedback_timeout"
)' /var/log/mine-teleop/vcu-can.jsonl*
```
