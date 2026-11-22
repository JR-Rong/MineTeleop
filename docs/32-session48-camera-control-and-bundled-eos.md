# session-000048：关键相机阻止控制与单路 EOS 停止全部视频

证据来自 `service-2d5b504aeef3ccc466df1b40_session-000048(1).zip`。
下列时间为 America/Los_Angeles，2026-10-11（UTC-07:00）。用户确认八路均级联
连接相机；日志中的 ID 已改为 `ccg2_channel_0` 至 `ccg2_channel_7`。相机名称
改变不证明物理接线、供电或 SerDes 状态已经正常。

## 直接控制原因：channel-7 没有稳定交付 V4L2 图像

八路都设置了 `critical_for_control=true`。日志显示：

| 时间 | 证据 |
| --- | --- |
| 19:01:35.592 | 会话开始 |
| 19:01:37.067–37.082 | channel-0 至 channel-6 收到第一帧；UYVY 1920×1080@30、stride 3840、完整 4147200 字节 |
| 19:01:39.890 | channel-7 首次 `timed out waiting for V4L2 frame`；captured/pushed/encoded 均为 0，触发 `vehicle_control_inhibited_by_camera` |
| 19:01:43.504 / 47.116 | channel-7 再次超时，依次重开；累计三次重开 |
| 19:01:49.343 | 第三次重开后收到第一帧，记录恢复；同一会话的控制禁止锁存仍保留 |
| 19:01:51.182 | DataChannel 打开，但 `adapter_ready=false`、`control_inhibited=true`；随后按安全逻辑关闭控制 DataChannel |
| 19:01:58.544 | channel-7 第四次超时，累计仅 captured/pushed/encoded=27；重试预算耗尽 |
| 19:01:58.653 | channel-7 停用，控制继续禁止 |

车端 trace 共记录 2753 次 `receive_apply`，接受数为 0：47 次
`control_service_unavailable`、2705 次 `control_inhibited`，结束时一次
`runtime_stop_requested`。所有请求均为 N、零油门、零转向、非 ESTOP。驾驶端
2778 条 native trace 均为 `send_completed`，没有 ACK 误重连事件。

因此本会话是关键相机阻止控制准入，没有成功应用驾驶命令；不是已经正常驾驶
后因 800 ms 控制断流停车。相机恢复只恢复视频，不自动清除同一会话的安全
锁存或重新授予驾驶权限。该行为与既有 `CriticalCameraControlLatch` 和首次
关键相机故障立即禁止控制的回归一致。

日志中的 `startup_timeout_ms=15000` 是编码新鲜度准入等待的上限；当前采集
异常路径会在首次读取超时（约 3 秒）立即锁存故障，不会等到 15 秒再处理。
本次修复不改变该行为、关键相机选择、握手或控制安全门限。

channel-7 的 27 帧也全部被驾驶端解码，RTP packets_lost=0；最初故障发生时
尚无帧进入编码器。因此直接原因在采集交付边界，不能归因于解码、网络丢包
或新的编码队列反压。硬件、GMSL/SerDes、采集卡 DMA 与内核驱动之间的具体
原因，仍需要板卡状态和系统日志区分。

## 新发现的软件缺陷：失败相机的 EOS 进入共享 RTP 会话

channel-7 停用前后，其他七路的表现不同于采集断帧：

- 19:01:58.467，七路 captured 约 642，encoded 约 240–242，发送队列正常排空。
- 19:02:03.834，七路 captured 约 803，encoded 停在 248–250，发送队列深度 0，
  编码前 appsrc 队列为 2。
- 至 19:04:24.674，captured 已增加到约 5028，encoded 和发送队列反压计数始终
  没有再增加。驾驶端各路 packets_received/frames_decoded 同样不再增加，FPS=0。

这说明另外七路仍在采集，整组媒体发送却停止。前期其他路有约 0.25%–1.74%
RTP 丢包，不能用它解释车端编码计数停止且采集仍持续增加。

源码在失败相机重试耗尽时向该 appsrc 发 EOS。`webrtcbin bundle-policy=max-bundle`
把多路 RTP 经过一个 `rtpfunnel` 送入共用会话；GStreamer 1.28.5 的 rtpfunnel
会把单路 EOS 向公共出口转发，结束其他路共用的发送路径。
参见上游 [webrtcbin](https://github.com/GStreamer/gstreamer/blob/1.28.5/subprojects/gst-plugins-bad/ext/webrtc/gstwebrtcbin.c)
和 [rtpfunnel](https://github.com/GStreamer/gstreamer/blob/1.28.5/subprojects/gst-plugins-good/gst/rtpmanager/gstrtpfunnel.c)。

受控 Docker / GStreamer 1.28.5 实验以两路真实 appsrc RTP 输入共享 rtpfunnel，
不使用相机、编码器、TURN 或外部网络：

| 处理 | 健康路在另一条 EOS 后发送 | 健康路收到 | 共享输出 EOS |
| --- | ---: | ---: | --- |
| 原策略，转发单路 EOS | 20 | 0 | true |
| 隔离单路 EOS | 20 | 20 | false |

## 修复与验证边界

修复在每路 RTP payloader 输出处隔离运行中的单路 EOS，避免结束共享 transport。
失败相机仍结束自己的采集、编码及录像分支，关键相机失败仍禁止控制。整个媒体
流水线关闭前已经设置 `stop_requested=true`，此时正常转发 EOS，保留原有退出
与录像收尾路径。探针只处理事件，不增加视频包处理、线程或锁。

新增实际 GStreamer 多路汇流回归，验证失败路 EOS 后健康路 20/20 继续交付，
以及 whole-pipeline shutdown 仍可正常结束；恢复旧的事件转发时应失败。

本次在 Docker Ubuntu 24.04/arm64、GStreamer 1.28.5 构建通过。首次并行
CTest 为 23/24，仅未修改的独立底盘握手诊断目标失败；它不链接媒体代码，
单项复测通过。最终源码完整串行 CTest 为 24/24 通过，没有调整底盘源码、
测试断言或安全门限。只在 Docker 恢复旧 EOS 转发后，新增回归按预期失败，
健康路收到 0/20，其他 70 个 core 测试通过；恢复修复并重新构建后完整回归通过。
测试副本与工作区的三份代码文件 SHA-256 一致，`git diff --check` 通过。
随后 Ubuntu 22.04/arm64、GStreamer 1.20.3 的最终完整串行 CTest 同样为
24/24 通过。`e844a88` 的车端 CI 首次失败于基础测试镜像未安装 rtpfunnel：
该阶段只有 base 插件。已补入 `gstreamer1.0-plugins-good` 测试依赖，并沿用
已有两秒取样等待预算保留全部断言。生产的编码器、安全门限和最终 bundled
GStreamer 版本不变；当前 x64 完整包结果以最新提交 CI 为准。

本包 manifest 标记 `complete=false`，驾驶端报告日志队列丢弃，runtime 含缺少
UTC 的记录；`vcu-can.jsonl` 为 0 字节，扫描的 CAN 轮转范围早于当前会话。因此
不能验证现场 CAN 输出、机械停车或实际驻车效果。19:04:29.724 的 SCTP 错误
发生在会话结束附近，不能解释 19:01:39 的采集故障或 19:01:58 的全路停画面。

下一步应分别验证媒体隔离修复和 channel-7 采集链路。后者需要故障附近的
板卡 frame_count/lock/ready、FIFO/AXI 错误计数及 V4L2/DMA 内核日志；修复媒体
EOS 不会修好 channel-7，也不会自动解除其控制禁止。
