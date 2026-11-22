# session-000049：channel-2 花屏、断帧、反馈过期与控制锁定

证据为 `service-2d5b504aeef3ccc466df1b40_session-000049.zip`。用户确认花屏
相机为 channel-2。下列时间为 America/Los_Angeles，2026-10-11（UTC-07:00）。

## 可以确认的故障顺序

| 时间 | 日志证据与含义 |
| --- | --- |
| 23:35:15.338–15.384 | 八路均交付第一帧。UYVY 1920×1080@30，完整 4147200 字节、stride 3840。 |
| 23:35:17.158 | VCU 适配器和控制 DataChannel 打开，尚处于 standby，未完成驾驶握手。 |
| 23:35:20.511–23:37:10.095 | channel-2 captured 从 81 增至 2007，同期其他路从约 155 增至 3442；该路实际交付明显不足 30 FPS，source sequence gap 为 0。 |
| 23:35:32.652 | 原生控制 ACK 实际连续 561 ms 未推进，触发 500 ms 重连保护；最老未确认命令 625 ms。它发生在关键相机故障之前。 |
| 23:37:15.129 | channel-2 第一次 `timed out waiting for V4L2 frame`，captured/pushed=2037、encoded=1830；关键相机安全锁存禁止控制。 |
| 23:37:15.165–15.189 | CAN 日志锁存 `software_fault / critical_camera_failed`，执行 emergency_stop、disarm_complete，随后适配器会话结束。 |
| 23:37:16.518 | 单路重开恢复视频，但同一会话控制锁存保留。 |
| 23:37:19.597 | 时间同步不确定度超过 25 ms，媒体流水线结束；下一次状态显示 RTT=55 ms、uncertainty=28 ms。 |
| 23:37:23.924 起 | 媒体重建并重新协商；八路计数重置，控制仍被关键相机锁存禁止。 |
| 23:37:53.179 / 23:38:22.531 / 23:38:31.414 / 23:38:35.030 | channel-2 再次反复采集超时，重开预算耗尽。 |
| 23:38:35.139 | channel-2 停用，第二个媒体流水线该路累计 captured=760、encoded=642。 |
| 至 23:40:46.836 | 其他七路持续采集和编码，encoded 增至约 5027–5088；上次的单路 EOS 隔离修复没有再拖停整组视频。 |
| 23:40:49.476 | SCTP association error，接近用户结束会话时间 23:40:49.547，不能解释先前的采集超时。 |

车端共有 5045 条 receive_apply，全被拒绝：716 条 session_profile_not_ready、
1090 条 vcu_handshake_not_ready、3207 条 control_inhibited、31 条
control_service_unavailable，以及结束时 1 条 runtime_stop_requested。没有驾驶命令
被接受，不应把本会话描述为已经正常驾驶后触发了 800 ms 控制 watchdog。
故障前的 CAN 快照有有效反馈、零速、EPB 四路为 2、VMC fault code 为 0；
这些只证明报告的状态，不能代替机械停车验收。

ACK 停滞区间，部分命令驾驶端到云端入队的 UTC 差值增至约 317–464 ms，
云端 mailbox 到发送为约 5–27 ms，车端控制 mutex 等待和处理为 0 ms。
这支持实际传输/排队异常，不是旧的“最老未确认命令年龄被当成 ACK 未推进”
误判。UTC 跨机器差值有时钟不确定度，不能据此细分 TCP、无线链路或操作系统
调度；控制门限和 ACK 保护未调整。

## 花屏源头仍不能唯一定位

channel-2 存在真实 V4L2 交付不稳定，首次故障前已有采集不足，重开仅暂时恢复。
超时指向相机/级联链路/SerDes/板卡 DMA/内核驱动到应用的交付边界；当前日志
不能区分这些层，也不能判断完整长度的原始像素是否已经损坏。

驾驶端同时报告该路丢包、丢帧和 PLI 请求。第一次流水线在 23:37:18.476
为 packets_received=18376、packets_lost=158、frames_received=1501、
frames_decoded=1398、frames_dropped=88、PLI=3；第二次流水线停用该路后
为 packets_received=4780、packets_lost=12、frames_decoded=462、frames_dropped=96。
这些说明传输/接收确有异常，可能破坏 H.264 预测链，不能证明所有花屏都来自网络。
仅有 JSON，没有原始图像、编码码流、系统 journal 或板卡状态，无法排除采集像素、
编码器或驾驶端解码器的问题。

旧浏览器指标多次报告 TURN，RTT 有数百至上千 ms，但没有记录已选 candidate pair；
旧算法还可能选中只做过成功检查的备用 pair。因此具体通路和每次 RTT 需要新的
已选 pair 诊断确认，不把旧标签当作直接通路证据。

## 已确认并修复的统计/显示缺陷

1. “链路”数值实际是 `Date.now() - vehicleTelemetry.sent_at_utc_ms`，即最后遥测
   的年龄；适配器/控制通道停止报告后会增长到数十秒，不等于 RTT 增长到数十秒。
   浏览器使用本机时间，而遥测使用云端同步时间；本次驾驶端 offset 约 -650 至
   -673 ms，旧计算还会把健康反馈误判为超过 500 ms。现在分别记录单调时钟的
   接收年龄和同步时间的源记录年龄，页面明确显示“遥测 N ms 前收到”，保留过期警示。
2. NVENC 给输出 PTS 和输出 segment 增加约 1000 小时偏移。旧的编码时延计算
   直接把 PTS 加到会话开始时间，并把负时延钳制为 0，掩盖了真实编码排队。
   现在通过输出 segment 转成 running-time 再计算，缺失/无效样本明确计数。
   参考 [NVENC 实现](https://github.com/GStreamer/gstreamer/blob/1.28.5/subprojects/gst-plugins-bad/sys/nvcodec/gstnvencoder.cpp)
   与 [GstSegment 时间转换](https://gstreamer.freedesktop.org/documentation/gstreamer/gstsegment.html)。
3. ICE 指标优先使用 transport.selectedCandidatePairId；兼容旧浏览器时只接受
   nominated 且 succeeded 的 pair，不再随便选中一次成功检查的备用 pair。
   无法确认的 RTT/时延显示未知。参考 [W3C WebRTC stats](https://www.w3.org/TR/webrtc-stats/#dom-rtctransportstats-selectedcandidatepairid)。

真实 GStreamer 1.28.5/x264 的受控实验得到输出 PTS=3600000000 ms、
segment.start=3599999900 ms、running-time=100 ms；在 elapsed=150 ms 时，
旧计算为 0 ms，修正后为 50 ms。这验证通用编码器时间戳偏移处理，未使用现场 GPU。

## 新增诊断与采集边界

- 每路首次配置记录实际 encoder factory、GStreamer 版本和诊断 schema。
- 每路最多每五秒一个 capture sample：读取耗时、帧交付间隔、最大间隔、计数、
  payload 长度、V4L2 driver/card/bus_info、buffer flags/index、设备 timestamp、
  poll revents、DQBUF EAGAIN 计数、bytesused/stride/sizeimage。buffer 元数据
  明确属于最后一次 DQBUF，不能把超时前的旧 buffer 当作本次读取成功；故障时保留同样字段。
- 媒体状态记录最近采集/编码年龄、时延有效/无效样本数和输出 PTS/segment。
- GStreamer WARNING 不再被丢弃，记录来源元素和限长 debug；前十条后限频为五秒。
- 时间同步越界事件带实际同步状态与门限。
- 驾驶端每五秒或健康状态变化记录 driver_feedback_health；媒体指标带已选 ICE pair、
  区间丢包、freeze、累计解码/处理耗时和最近包接收时间。

诊断不增加视频录制、逐帧像素输出、外部系统命令、采集线程或视频包探针。
使用现有日志与安全逻辑；200/300/800 ms 控制门限、25 ms 时间同步门限、
关键相机安全锁存、固定 VCU 握手和单路 EOS 隔离均保留。

本包 manifest 为 complete=false：云端/驾驶端报告日志队列丢弃；runtime 含缺少
UTC 的记录；CAN 扫描达到上限，末尾止于 23:37:15.189（与适配器关闭时间吻合）。
不能宣称会话末尾仍有完整 CAN 覆盖。部署路径的包名为
`mine-teleop-vehicle-ubuntu22.04-x64-20261012-033453`，与 b17f7f5 CI 产物名称吻合；
日志没有可验证的包 hash/部署 SHA，因此包名仅为版本线索。

## 验证与下一步

新增回归覆盖偏移 segment、无效 PTS、未来帧、浏览器时钟偏差、40 秒反馈未更新，
以及实际 ICE pair 与备用 pair 的选择。驾驶端逻辑 30/30 通过。
Docker Ubuntu 24.04/arm64、GStreamer 1.28.5 首轮 CTest 24/24 通过；后续完整
复测出现两个未修改的过期/心跳时序测试失败；仅在 Docker 恢复未修改的
server.cpp 后，过期测试也会失败，未调整这些断言。恢复最终源码并构建后，完整串行 CTest 24/24 通过；最终八份代码与测试容器哈希一致，再次完整
串行验证为 24/24（58.22 秒）；
仅在容器撤销 segment 转换时，新增回归按预期失败，其他 71 项 core 测试
通过；恢复修复后通过。5ad08b8 的 Linux/车端 CI 首次失败于共享 JS helper
新增的可选链语法不兼容 Node 12；已改用同等逻辑的普通空值检查，保留旧环境
和全部断言。最终包与跨平台结果以最新提交 CI 为准。

页面 QA 使用 Docker 构建输出的实际页面，本地 URL http://127.0.0.1:43591/；
所有 API 请求由模拟响应截获，不连接现场。Browser skill 未提供，使用现有
Playwright 1.62.1 与 Chrome，视口 1440×1000、390×844。

| 检查 | 结果 |
| --- | --- |
| 页面身份、非空、无框架错误覆盖 | 通过，Mine Teleop WebRTC Console |
| 控制台错误/告警 | 0 |
| 点击“收到新遥测” | 0 ms 前收到，有效状态；模拟 -40 秒时钟偏差不产生误报警 |
| 点击“模拟40秒无更新” | 40500 ms 前收到，过期状态；不再标成链路延时 |
| RTT 和视频时延为 null | 显示未知 |
| 截图与移动端布局 | 已检查；测试截图位于临时验证目录，未加入仓库 |

软件统计修复与诊断需要更新驾驶端和车端，云端无需因本次修改替换。要确定
channel-2 的原始花屏与断帧源头，还需同一故障窗口的板卡 frame_count/lock/ready、
FIFO/AXI 错误计数、V4L2/DMA 内核日志，以及安全空闲时采集的原始帧。
新日志用于确定应用读取、设备交付、编码输出和接收解码的先后异常，不代替
板卡/内核/像素证据，也不会自动解除本会话的控制锁定。
