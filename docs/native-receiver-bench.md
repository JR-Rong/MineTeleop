# PR25 原生 WebRTC 接收台架验证

本探针用于把 Sunshine/Moonlight 的“硬件解码、有限队列、分阶段统计”思路落到现有 WebRTC 链路上。它是媒体实验入口，不是已完成的桌面控制端。PR 的基线和合入目标都是 `rjr/cx/orin-isx031-cpu`（PR25），不改变产品默认传输或安全策略。

## 实现与范围

- 复用 `VehicleMediaRuntime`、`SignalingService` 和 `DriverConsoleRuntime`，走真实的 SDP/ICE/DTLS/SRTP；没有另写视频 UDP 协议。
- 同机模式创建仅监听回环地址的临时信令服务，随机生成凭据；结束后回收本次会话和线程。跨机模式通过回环 SSH 隧道接入单独启动的台架信令服务，凭据从环境变量读取。产品信令配置要求至少一个 ICE URL，实验填入回环 discard 端口 `stun:127.0.0.1:9`，不配置 TURN。直连结论以 libnice 的实际选中候选对为证。
- GStreamer 1.20 的 `get-stats` 候选对字段不完整，使用 `new-selected-pair-full` 记录实际候选类型及 UDP/TCP，保留 RTP/jitterbuffer 原始统计供核查。
- H.264 解码可选 `avdec_h264`（2 线程）、Jetson `nvv4l2decoder disable-dpb=true` 或 Mac `vtdec_hw`（只允许硬件解码）。默认仅消费 sample；`--display` 增加单帧泄漏队列与 `glimagesink sync=false`，记录 GL 绘制回调。它不读取像素，也不声称测到了扫描输出。
- 接收端 `webrtcbin latency` 可调；解码输出 `appsink max-buffers=1 drop=true sync=false`。独立消费线程可以人为暂停，以检验“丢旧帧、追新帧”的行为。
- 拒绝控制开启、非 mock、非 bench、非零速度上限、录像/上传开启的本地配置。跨机模式不能仅凭本地配置证明远端安全状态，必须另行检查实际发送端配置、相机占用和 CAN 状态。控制输入、DataChannel 命令转发、CAN、驾驶授权和屏幕扫描输出均不在本次验收范围。
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

## 浏览器/原生 A/B 方法

本次同机收发验证不回答“改成独立客户端能减少多少波动”。要回答它，需要在同一台实际控制电脑上交替运行浏览器和原生接收器，保持车辆端、相机数、码率、分辨率、FPS、编解码硬件、显示刷新率、实际选中网络路径一致。至少记录正常负载、UI/CPU 负载、带宽收缩、短时丢包和断链恢复各场景的 p95/p99/max、冻结时长及恢复时间。

浏览器 RTCStats 差分得到的是一个采样周期的平均值，原生探针报告的是逐帧分布，二者不能直接比较 p99；比较时需统一采样粒度，或使用同一种呈现/光学测量。

玻璃到玻璃延迟需外部光学测量，或经验证的采集时间戳和时钟误差界限；不能拿 `rtt/2 + 平均解码耗时` 当真实端到端延迟。原生显示还需要验证窗口合成、VSync、硬件表面到呈现的路径。原生控制热路径需要另测输入到车辆接受/执行的阶段时间，并完整复用现有签名、会话/代际围栏、100 ms 转发有效期、超时制动和恢复逻辑。

收益判断应分开：独立消费线程和有界队列可以隔离 UI 卡顿造成的积压；它们不能消除相机/编码器争用、5G 网络波动或中继绕路。本 PR 不改变编码码率控制，也没有添加 FEC、拥塞控制或新的打洞方案。

## 2026-10-27 Orin 实测

实现提交 `a2443ddf4eeb23453e18edbdf3c7be3d18e2ebec`，最终探针二进制 SHA-256 为 `fce1aef4fa2cd84513583ebc4f42eb7c4e0a017e92e9bdd5f111866d5a1ad9c3`。完整指标、原始报告摘要校验值、竞争进程检查及验证范围见 [证据 JSON](evidence/native-receiver-20261027.json)。

三轮各请求 30 秒，均使用同一台 Orin 收发、双 ISX031、CPU x264 编码和 NVIDIA 解码，实际选中 host↔host/UDP。每秒竞争进程检查未检出重叠；相机最终无占用、CAN 始终 DOWN。另有两次测试因发现 FrameForge/ncu 进程而主动中止，已排除。25 W 电源模式为观察值，本任务未调整时钟或功耗配置。

| 实验 | 两路唯一 RTP 时间戳 / 唯一解码 PTS | depay→decode p99（ms） | decode→consume p99 / max（ms） | 结论 |
|---|---|---|---|---|
| 默认 30 ms 缓冲 | 843/843；843/843 | 16.69；17.03 | 0.143/5.22；0.139/0.69 | 基线身份计数一致；解码输出间隔 p99 为 47.22/47.85 ms |
| 每约秒暂停消费 200 ms | 844/845；845/845 | 22.30；17.07 | 27.72/36.10；17.20/77.97 | 每路记录 28 次暂停，104/107 帧未被消费；第一路仍有 1 个身份计数差异 |
| 0 ms 缓冲 | 843/906；843/921 | 35.15；34.07 | 0.119/1.88；0.134/1.70 | 出现明显身份计数差异，不能用于证明 FPS/延迟改善 |

这些分布包含启动和退出阶段，没有人为剔除尾部。暂停实验中 depay→decode 的最大值达到 **138.46 ms**；消费者取到的旧 sample 最大等待 **77.97 ms**。单 buffer 能防止队列持续增长，但输入停止时留下的最后一帧仍会变旧，不能只靠队列长度保证新鲜度。

硬件解码输出中观察到重复 PTS，最终探针每路过滤 1–4 个重复 buffer；真实 GStreamer 回归验证了重复/无效 PTS 不会到达下游。0 ms 实验在过滤后仍有比唯一 RTP 时间戳更多的不同解码 PTS，说明“PTS 不同”也不足以证明是新摄像头帧。它与少量重复 PTS 是两个问题，后者已被探针处理，前者仍需追踪 RTP 重复/恢复与时间戳转换路径。保持探针默认 30 ms，不依据这个实验修改产品缓冲策略或宣称性能增益。

发送端三轮均出现约 **1.92 秒**的启动期 capture→encode 最大值，每路既有发送新鲜度保护丢弃了 1 个过期 AU；整个窗口的 capture→encode 平均值约 **67.6–67.8 ms**。它包含主机开始取帧等等待，不是传感器曝光延迟。实际输出数、启动开销和尾部峰值均保留，不能用约 30 fps 的接收输出窗口掩盖启动延迟。

可确认的借鉴点是：按阶段测量、有界消费队列、明确的帧身份、实际连接对取证，以及保留尾部/异常而非只看平均值。此次没有证明“独立客户端能大幅降低整体波动”；仍应先完成同一控制电脑、同一硬件解码和显示条件下的浏览器/原生 A/B。

验证情况：ARM64/GStreamer 1.20.3 构建成功；104 项浏览器控制逻辑测试和 2 组原生探针测试通过。65 组本地 CTest 中累计 64 组取得通过结果：首次测试环境中的旧 Node、非回环隔离问题已修正，校时敏感 suite 单独复测通过；`mine-teleop-product-direct-integration-tests` 仍报 `DC not qualified`，未修改 PR25 对照也失败。CI 增加 `libnice-dev` 并收集新探针产物；硬件采集和负载实验不注册为自动测试。

## Mac 跨机接收模式

Mac 构建关闭 Linux 车辆运行库，开启同一个接收探针。需要 GStreamer 1.22+、libnice、VideoToolbox 插件和 GL sink；`vtdec_hw` 不会静默回退软件解码。

```sh
cmake -S . -B build-mac-receiver \
  -DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=OFF \
  -DMINE_TELEOP_BUILD_BENCH_PROBES=ON \
  -DMINE_TELEOP_BUILD_TESTS=ON
cmake --build build-mac-receiver --target mine-teleop-native-receiver-probe -j4
ctest --test-dir build-mac-receiver -R 'native-receiver|receiver-ab-analyzer' --output-on-failure

# Orin 上先单独启动 loopback 信令和 control_enabled:false 的 vehicle-runtime。
# 信令的 driver ID 为 native-bench；密码、设备令牌使用本轮临时环境变量。
# 仅转发信令，媒体仍由 ICE 选择局域网 UDP。
ssh -N -L 127.0.0.1:18766:127.0.0.1:18765 orin

# 在另一个终端设置 MINE_TELEOP_DRIVER_PASSWORD（不写入报告或命令参数）。
build-mac-receiver/mine-teleop-native-receiver-probe \
  --config /absolute/safe-bench-copy.yaml \
  --signaling-url http://127.0.0.1:18766 --driver-id native-bench \
  --decoder vtdec_hw --display --jitter-ms 30 --seconds 60 \
  --output /absolute/new-native.json
```

每轮重启台架信令/发送进程，避免复用前一轮会话；开始前和运行中监测竞争进程，异常时只停止本任务进程。远端真实配置必须同时验证 control disabled、mock、bench、零速度、录像/上传关闭以及 CAN DOWN。接收器本地配置校验不能代替这一步。Mac 因系统盘空间不足，实测运行库与构建均放在外置盘；镜像下载的 Homebrew bottle 对照官方元数据核对 SHA-256，未升级系统 Homebrew 依赖。

浏览器使用当前 PR 的 `mine-teleop-control` 页面与前台 Chrome。在两路视频播放后执行 [采样函数](../scripts/test/receiver_ab_browser.js)，60 秒自动停止；导出 `window.receiverAB` 的 JSON（函数属性不参与 JSON 序列化）。该函数只观测 RTCStats、视频帧回调、页面可见性和长任务，发现断开/换 peer 会提前结束并保留错误。导出后离开页面释放会话，再运行原生端。分析器统一截取各轮第 10–50 秒，完整原始数据仍保留启动和结束阶段：

```sh
python3 scripts/test/receiver_ab_analyze.py browser.json native.json > summary.json
```

浏览器解码间隔的均值/标准差由 `totalInterFrameDelay`、`totalSquaredInterFrameDelay`、`framesDecoded` 的区间差分计算；原生使用同一窗口的逐帧解码输出时间。此处不把一个周期的平均解码耗时 p99 与逐帧 p99 比较。浏览器 `presentedFrames` 的增量用于呈现计数，回调次数只表示采样覆盖率；原生 GL draw 与浏览器 compositor presentation 都不能证明像素扫描到屏幕的时刻。分析回归用“30 fps、JS 只回调 15 次/秒”的样例保证不会误判成 15 fps 掉帧。

## 2026-10-27 Orin → M4 Mac A/B 结果

结论：这组局域网初测观察到原生接收端的解码输出节奏略稳，尚未证明换客户端能大幅降低整体波动。正式对照采用 B1 → A2 → B2 → A5 四轮（B 为原生，A 为浏览器），各采样 60 秒，表内统一使用第 10–50 秒的稳定窗口。原始报告哈希、完整窗口统计、监控结果与排除记录见 [Mac A/B 证据](evidence/mac-receiver-ab-20261027.json)。

固定发送端为 PR25 Orin 双 ISX031，1920×1536 UYVY 采集后输出两路 960×768、30 fps、每路 2.5 Mbps CPU H.264。接收端为同一台 Mac mini M4 / 16 GB；Chrome 156.0.8078.48 前台页面对比 GStreamer 1.28.8 + `vtdec_hw` + GL sink。没有改默认产品参数，原生 jitter target 为 30 ms，浏览器保持自适应策略。只转发信令，两端媒体均为同一 Orin `gbe1` 局域网链路上的 UDP，无 TURN；B1 选择 IPv6 link-local，B2/浏览器远端选择 IPv4，不宣称候选地址逐项一致，也不证明公网穿透。

| 轮次 | 两路解码 fps | 解码帧间隔标准差（ms） | 呈现/绘制计数 fps |
|---|---|---|---|
| 原生 B1 | 29.99 / 29.99 | 3.19 / 3.13 | GL draw 30.00 / 30.00 |
| 浏览器 A2 | 30.00 / 30.00 | 4.66 / 4.71 | presentedFrames 29.97 / 29.92 |
| 原生 B2 | 30.00 / 30.00 | 3.23 / 3.25 | GL draw 30.00 / 30.00 |
| 浏览器 A5 | 29.97 / 30.00 | 4.86 / 4.06 | presentedFrames 29.99 / 29.92 |

浏览器窗口内新增解码丢帧、RTP 丢包及冻结计数均为 0；平均实际 jitterbuffer 等待为 **3.41–5.62 ms**，目标平均为 **12.65–15.24 ms**。浏览器解码均值约 0.8 ms。原生 depay→decode 约 1.4 ms，包含解析，不能与浏览器 `totalDecodeTime` 当成同一边界比较。原生两轮 RTP 唯一时间戳与解码帧数分别为每路 1744/1744、1743/1743，无重复解码 PTS；jitterbuffer 原始报告没有丢包、迟到或重复包。解码→GL draw p99 为 **1.29–1.39 ms**，它不包含网络、jitterbuffer、窗口合成和屏幕扫描。

浏览器 RVFC 的覆盖率只有 **73%–89%**，而 `presentedFrames` 仍接近 30 fps。因此回调间隔 p99 的约 67–68 ms 不能被表述成等量的屏幕卡顿。采样结束后额外测量 120 个 rAF 间隔，平均 **22.22 ms（约 45 Hz）**，与系统显示模式报告的 180 Hz 不同。该现象可解释部分回调节奏，但原因尚未定位，不能据此断言是浏览器架构或显示器故障。CDP 确认 Chrome 使用 Apple M4 / ANGLE Metal、GPU 合成及视频解码功能启用；本次 RTCStats 未暴露实际 decoderImplementation / powerEfficientDecoder，因此没有证明浏览器与原生使用相同的实际解码实现。

排除记录：A1 在 60 秒采样结束约 40 秒后，台架监控发现 compute-sanitizer；虽未与采样窗口重叠，正式对照仍保守排除。A3 被测试服务的 180 秒总时限截断，采样器记录断开错误，已排除并把服务保护时限调整为 300 秒。A4 启动前发现竞争实验，未连接相机。四轮纳入数据的服务监控均未检出竞争；A5 开始前另要求连续空闲 15 秒，并增加 nsys 检查。监控为每秒采样，不能排除短于采样间隔的外部工作。最终相机无占用，本任务进程退出，CAN 保持 DOWN。

这些是短时、正常局域网、媒体专用原型的结果。原生端没有完整控制台 UI；未做 UI 压力、5G 丢包/拥塞、长时间运行、重连、原生控制输入或光学端到端测试。Mac 系统盘接近满载且有较多既有 swap，其他 Mac 应用负载未完全控制。下一步应先对齐实际显示节奏和硬件解码证据，再做相同负载下的呈现/光学延迟测试，而不是直接以本表承诺产品收益。

本次最终 Mac 构建成功，3 组定向 CTest（分析口径、真实 Gst PTS 过滤、配置/参数隔离）和 104 项控制逻辑测试通过。Linux 本地容器无法启动，报 `/etc/resolv.conf: input/output error`，没有把上一版 ARM64 构建结果当成本次通过；新提交的 Linux 验证以 CI 为准。复核复用了既有 DriverConsoleRuntime/会话围栏和同一接收循环；新观测均有界且只在显式开启的探针中运行，未增加产品热路径负担。

## Mac 混合 UI 台架（PR26 后续）

`desktop/bench/main.cjs` 是独立、显式启动的实验入口：同一 Electron 44.3.0、同一控制台页面、窗口尺寸和压力脚本，`browser` 使用原有 WebRTC `<video>`，`native` 通过 Node-API 将 VideoToolbox/GL 视图嵌入相机区域。接收器与命令队列运行在独立线程，页面继续复用产品登录、会话围栏和信令。原生帧不会被伪造成产品的健康视频准入，台架关闭控制。此入口和 `.node` 不进入现有桌面发布包，也不改变默认客户端。

原生解码与探针共用 `cpp/tests/native_receiver_core.hpp`。macOS 嵌入模式为每路创建独立 GL display context，避免 VideoToolbox GL 分配器经公共管线发现并共用另一视频轨道的 drawable；仅有两路 draw 回调不能证明两个区域都显示。必须同时检查窗口截图和两路 NSView 的子视图挂载。绘制仍使用有界队列和 `sync=false`，未实现 Moonlight 的显示刷新调度。

构建需要 GStreamer GL 和 Node-API 头文件：

```sh
cmake -S . -B build-mac-receiver \
  -DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=OFF \
  -DMINE_TELEOP_BUILD_TESTS=ON -DMINE_TELEOP_BUILD_BENCH_PROBES=ON \
  -DMINE_TELEOP_NODE_API_INCLUDE=/absolute/node/include
cmake --build build-mac-receiver --target mine-teleop-native-video-layer mine-teleop-native-receiver-probe

# 先按前文启动独立、control_enabled:false 的远端台架及本地控制服务。
# MINE_TELEOP_DRIVER_PASSWORD 仅由环境传入，不写入证据。
electron desktop/bench/main.cjs --mode native \
  --url http://127.0.0.1:18081/ --config /absolute/safe-bench.yaml \
  --probe /absolute/build-mac-receiver/mine-teleop-native-receiver-probe \
  --addon /absolute/build-mac-receiver/mine-teleop-native-video-layer.node \
  --output /absolute/new-run-directory --seconds 600
```

A 组仅将 `--mode` 换为 `browser`。支持 15–1800 秒；阶段分析要求超过 50 秒，正式对照使用 600 秒。五个等长阶段为：空闲、每 100 ms 约 8 ms DOM/CPU 工作、每 100 ms 约 40 ms 工作、每约秒 200 ms 主线程停顿、恢复空闲。实际工作时间/次数、长任务、可见性与焦点变化均保存，不只记录预设值。阶段边界前后各留 5 秒，阶段内部尾部不删除。三轮以 B→A→B 顺序观察前后漂移，不能替代随机化、多设备重复实验。

`ui.json` 保存 RTCStats/视频呈现观察与相同的 UI 负载；`native.json` 保存独立线程逐帧证据；`metadata.json` 和 `events.jsonl` 保存版本、显示环境、异常和原生视图挂载。`web-content.png` 只截图网页合成结果，不包含原生 NSView，不能用它验收原生视频像素。

```sh
python3 scripts/test/hybrid_receiver_analyze.py /absolute/run-B1 /absolute/run-A /absolute/run-B2
```

原生新增时间点是 SRTP 解密后、jitterbuffer 输入处的 RTP 到达时间，按帧末包/PTS 关联到解码及 GL draw；这不是相机采集时间或内核收包时间。浏览器 `receiveTime→presentationTime` 结束于 compositor submission，且 JS 的 RVFC 在 UI 卡顿时会漏采样，必须报告覆盖率。两种端点并不完全相同，优先比较各客户端在 UI 负载前后的变化；不得将两列相减宣称相机到屏幕改善了同等毫秒数。原生 30 ms jitter target 和浏览器自适应缓冲仍保留差异，本实验比较整条接收媒体路径，不能把收益全部归因于渲染器。
