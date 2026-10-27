# PR25 原生 WebRTC 接收台架验证

本探针用于把 Sunshine/Moonlight 的“硬件解码、有限队列、分阶段统计”思路落到现有 WebRTC 链路上。它是媒体实验入口，不是已完成的桌面控制端。PR 的基线和合入目标都是 `rjr/cx/orin-isx031-cpu`（PR25），不改变产品默认传输或安全策略。

## 实现与范围

- 复用 `VehicleMediaRuntime`、`SignalingService` 和 `DriverConsoleRuntime`，走真实的 SDP/ICE/DTLS/SRTP；没有另写视频 UDP 协议。
- 每次运行创建仅监听回环地址的临时信令服务，随机生成凭据；结束后回收本次会话和线程。产品信令配置要求至少一个 ICE URL，实验填入回环 discard 端口 `stun:127.0.0.1:9`，不配置 TURN。直连结论以 libnice 的实际选中候选对为证。
- GStreamer 1.20 的 `get-stats` 候选对字段不完整，使用 `new-selected-pair-full` 记录实际候选类型及 UDP/TCP，保留 RTP/jitterbuffer 原始统计供核查。
- H.264 解码可选 `avdec_h264`（2 线程）或 Jetson `nvv4l2decoder disable-dpb=true`。后者保留 NVMM 缓冲，不增加 CPU 图像下载；本探针不读取像素，也不呈现到屏幕。
- 接收端 `webrtcbin latency` 可调；解码输出 `appsink max-buffers=1 drop=true sync=false`。独立消费线程可以人为暂停，以检验“丢旧帧、追新帧”的行为。
- 拒绝控制开启、非 mock、非 bench、非零速度上限、录像/上传开启的配置。控制输入、DataChannel 命令转发、CAN、驾驶授权和屏幕显示都不在本次验收范围。
- 不做会话恢复：遇到第二次 offer、异步管线错误或协商超时会报告失败，避免把重连前后样本混在一起。

## 构建与运行

需要项目原有依赖、GStreamer 1.20+ 开发包、`libnice-dev`，以及运行时 `webrtcbin`、`nice`、`x264enc`、所选 H.264 解码器。默认构建不开启探针。

```sh
cmake -S . -B build-native-bench \
  -DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=ON \
  -DMINE_TELEOP_BUILD_TESTS=ON \
  -DMINE_TELEOP_BUILD_BENCH_PROBES=ON
cmake --build build-native-bench --target mine-teleop-native-receiver-probe -j4
ctest --test-dir build-native-bench -R native-receiver --output-on-failure
```

先从 PR25 的 `configs/vehicle-agent.orin-cpu-bench.yaml` 复制独立的台架配置，将 `runtime.control_enabled` 明确设为 `false`。真实双相机使用 `configs/vehicle-agent.orin-isx031-bench.yaml` 的独立副本并作同样修改；保持其 mock、零速度、录像/上传关闭设置。运行前确认相机没有被其他任务占用，禁止并行运行其他性能实验。

```sh
build-native-bench/mine-teleop-native-receiver-probe \
  --config /absolute/bench.yaml --validate-only

build-native-bench/mine-teleop-native-receiver-probe \
  --config /absolute/bench.yaml --seconds 60 --jitter-ms 30 \
  --decoder nvv4l2decoder --output /absolute/new-baseline.json

build-native-bench/mine-teleop-native-receiver-probe \
  --config /absolute/bench.yaml --seconds 60 --jitter-ms 30 \
  --decoder nvv4l2decoder --consumer-stall-ms 200 \
  --output /absolute/new-consumer-stall.json
```

`--output` 必须是新文件。`--seconds` 为 5–300 秒，缓冲目标 0–200 ms，消费暂停 0–500 ms；暂停每隔约 1 秒注入一次。不要将暂停实验解释成网络丢包实验。相机初始化仍按 PR25 的 Orin 文档进行，不在探针中修改设备拓扑、网络路由或系统服务。

## 指标口径

| 字段 | 测量边界 | 不能代表什么 |
|---|---|---|
| `vehicle.lanes` / `vehicle.attempts[].lanes` | 现有捕获、编码与发送新鲜度统计 | 远端已经收到/呈现 |
| `depay_to_decode` | 同一个 PTS 的完整 H.264 AU 离开 depay 到解码器输出 | 包含网络和 jitterbuffer 的全接收耗时 |
| `decode_to_consume` | 解码输出到消费线程取到对应 sample | 像素处理或显示延迟 |
| `decoded_frame_gap` | 连续解码输出的单调时钟间隔 | 摄像头曝光间隔 |
| `consumed_frame_gap` | 连续消费的单调时钟间隔 | VSync/屏幕实际帧间隔 |
| `not_consumed_frames` | 解码数减消费数 | 全部都是网络丢帧；结束时未取走的帧也可能计入 |
| `selected_pair` | libnice 实际选中连接对 | 5G/CGNAT/跨网穿透成功率 |

延迟和帧间隔报告 p50/p95/p99/max、样本数；最多保存每组 131072 个样本，超额数显式记录。PTS 对应表有界，无法匹配的样本单独计数；另存 RTP 唯一时间戳、解码原始 buffer 数、唯一 PTS 数。重复/无效解码 PTS 在 pad 上被丢弃，不刷新新帧计数、帧间隔或等待年龄。接收统计延续到发送端退出和接收器停止，发送端 `lanes` 是停止前快照，二者不保证同一截断时刻；不能把两端计数直接相减作为网络丢帧数。`passed` 是本次台架收发验收标志，不等于生产或实车验收，也不证明原生优于浏览器。

浏览器诊断增加 `interval_decode_ms`、`interval_processing_ms`、`interval_jitter_buffer_ms`、`interval_jitter_target_ms`、`interval_frames_decoded`。它们由相邻 RTCStats 累计计数相减得到，首次采样、流身份变化、时间不前进、计数回退或缺少字段时返回 `null`，不伪造 0。原有累计指标和 `passed` 判定保持原样。 页面按现有节奏约每秒更新并提交运行时指标，浏览器事件日志仍约每 5 秒抽取一次快照；连续实验应按秒保存运行时样本，不能用抽样日志推断完整尾部。`totalProcessingDelay` 已包含接收处理等待，不与 jitterbuffer 延迟重复相加。

## 后续浏览器/原生 A/B

本次同机收发验证不回答“改成独立客户端能减少多少波动”。要回答它，需要在同一台实际控制电脑上交替运行浏览器和原生接收器，保持车辆端、相机数、码率、分辨率、FPS、编解码硬件、显示刷新率、实际选中网络路径一致。至少记录正常负载、UI/CPU 负载、带宽收缩、短时丢包和断链恢复各场景的 p95/p99/max、冻结时长及恢复时间。

浏览器 RTCStats 差分得到的是一个采样周期的平均值，原生探针报告的是逐帧分布，二者不能直接比较 p99；比较时需统一采样粒度，或使用同一种呈现/光学测量。

玻璃到玻璃延迟需外部光学测量，或经验证的采集时间戳和时钟误差界限；不能拿 `rtt/2 + 平均解码耗时` 当真实端到端延迟。原生显示还需要验证窗口合成、VSync、硬件表面到呈现的路径。原生控制热路径需要另测输入到车辆接受/执行的阶段时间，并完整复用现有签名、会话/代际围栏、100 ms 转发有效期、超时制动和恢复逻辑。

收益判断应分开：独立消费线程和有界队列可以隔离 UI 卡顿造成的积压；它们不能消除相机/编码器争用、5G 网络波动或中继绕路。本 PR 不改变编码码率控制，也没有添加 FEC、拥塞控制或新的打洞方案。
