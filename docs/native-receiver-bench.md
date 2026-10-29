# PR26 原生 WebRTC 接收与双栈直连验证

本探针用于把 Sunshine/Moonlight 的“硬件解码、有限队列、分阶段统计”思路落到现有 WebRTC 链路上。它是媒体实验入口，不是已完成的桌面控制端。PR 的基线和合入目标都是 `rjr/cx/orin-isx031-cpu`（PR25），不改变产品默认传输或安全策略。

## 实现与范围

- 复用 `VehicleMediaRuntime`、`SignalingService` 和 `DriverConsoleRuntime`，走真实的 SDP/ICE/DTLS/SRTP；没有另写视频 UDP 协议。
- 同机模式创建仅监听回环地址的临时信令服务，随机生成凭据；结束后回收本次会话和线程。跨机模式通过回环 SSH 隧道接入单独启动的台架信令服务，凭据从环境变量读取。产品信令配置要求至少一个 ICE URL。同机诊断目前使用无服务的回环高端口 `stun:127.0.0.1:34789`，不配置 TURN；它不具备公网地址发现能力。旧实验的端口 9 占位配置会触发 Chromium UDP socket 错误，已移除，见文末根因记录。IPv4 NAT 地址发现实验必须使用实际可达的公网 STUN 服务；公网 IPv6 host 候选本身不依赖 STUN。直连结论以实际选中候选对及独立出口证据为准。
- GStreamer 1.20 的 `get-stats` 候选对字段不完整，使用 `new-selected-pair-full` 记录实际候选类型及 UDP/TCP，保留 RTP/jitterbuffer 原始统计供核查。
- H.264 解码可选 `avdec_h264`（2 线程）、Jetson `nvv4l2decoder disable-dpb=true` 或 Mac `vtdec_hw`（只允许硬件解码）。默认仅消费 sample；`--display` 增加单帧泄漏队列与 `glimagesink sync=false`，记录 GL 绘制回调。它不读取像素，也不声称测到了扫描输出。
- 接收端 `webrtcbin latency` 可调；解码输出 `appsink max-buffers=1 drop=true sync=false`。独立消费线程可以人为暂停，以检验“丢旧帧、追新帧”的行为。
- 拒绝控制开启、非 mock、非 bench、非零速度上限、录像/上传开启的本地配置。跨机模式不能仅凭本地配置证明远端安全状态，必须另行检查实际发送端配置、相机占用和 CAN 状态。控制输入、DataChannel 命令转发、CAN、驾驶授权和屏幕扫描输出均不在本次验收范围。
- 独立 CLI 探针仍不做会话恢复。混合 UI 接收器允许测量开始前的新代际 offer，以验证直连失败后的回退；各次尝试分别保存。开始计时后发生 Peer 更换则终止该轮，不拼接重连前后样本。

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

`--output` 必须是新文件。`--seconds` 为 5–1800 秒，缓冲目标 0–200 ms，消费暂停 0–500 ms；暂停每隔约 1 秒注入一次。不要将暂停实验解释成网络丢包实验。相机初始化仍按 PR25 的 Orin 文档进行，不在探针中修改设备拓扑、网络路由或系统服务。

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

A 组仅将 `--mode` 换为 `browser`。支持 15–1800 秒；阶段分析要求超过 50 秒，2026-10-27 的历史局域网对照使用 600 秒；后续正式跨网对照要求每组 1800 秒。五个等长阶段为：空闲、每 100 ms 约 8 ms DOM/CPU 工作、每 100 ms 约 40 ms 工作、每约秒 200 ms 主线程停顿、恢复空闲。实际工作时间/次数、长任务、可见性与焦点变化均保存，不只记录预设值。阶段边界前后各留 5 秒，阶段内部尾部不删除。预设三轮以 B→A→B 顺序观察前后漂移，不能替代随机化、多设备重复实验。必须逐轮核对实际 ICE 传输协议；本次发现网页选中 TCP 后，又补充原生 TCP 长测，详见下面的结果与排除记录。

`ui.json` 保存 RTCStats/视频呈现观察与相同的 UI 负载；`native.json` 保存独立线程逐帧证据；`metadata.json` 和 `events.jsonl` 保存版本、显示环境、异常和原生视图挂载。`web-content.png` 只截图网页合成结果，不包含原生 NSView，不能用它验收原生视频像素。

```sh
python3 scripts/test/hybrid_receiver_analyze.py /absolute/run-B1 /absolute/run-A /absolute/run-B2
```

原生新增时间点是 SRTP 解密后、jitterbuffer 输入处的 RTP 到达时间，按帧末包/PTS 关联到解码及 GL draw；这不是相机采集时间或内核收包时间。浏览器 `receiveTime→presentationTime` 结束于 compositor submission，且 JS 的 RVFC 在 UI 卡顿时会漏采样，必须报告覆盖率。两种端点并不完全相同，优先比较各客户端在 UI 负载前后的变化；不得将两列相减宣称相机到屏幕改善了同等毫秒数。原生 30 ms jitter target 和浏览器自适应缓冲仍保留差异，本实验比较整条接收媒体路径，不能把收益全部归因于渲染器。


## 2026-10-27 相同 UI 负载的长测

实验保留现有控制台页面，将原生 VideoToolbox/GL 视图放在两个原有相机区域。相机、Orin PR25 CPU H.264 发送端、两路 960×768/30fps/2.5Mbps、Electron 44.3.0 / Chromium 152.0.7977.78、1500×960 窗口和五段 UI 压力脚本一致。系统报告显示模式为 3440×1440 / 180Hz，但这不是实际扫描输出的测量。3D 场景关闭；本次压力为 DOM/JavaScript 主线程工作，不覆盖 3D/GPU 饱和。

最初三轮各 600 秒，顺序为原生 UDP B1 → 网页 TCP A → 原生 UDP B2。事后核对完整 RTCStats 才发现协议不同，因此不能将这组三轮的差值直接归因于原生视频层。保留全部数据，并补测 600 秒原生 TCP B3，与网页 TCP A 比较。所有原始数据和源码/二进制哈希见 [长测证据](evidence/hybrid-ui-long-ab-20261027.json)；每个 120 秒阶段统计中间 110 秒，完整 600 秒未裁剪。

协议核对暴露了实验入口的问题：`default_public_and_private_interfaces` 只使用默认路由网卡，可能遗漏 Orin 的局域网接口，已改为 `default`。参见 [Electron 的接口枚举规则](https://www.electronjs.org/docs/latest/api/web-contents#contentssetwebrtciphandlingpolicypolicy)。随后增加了仅对本测试 origin 生效的局域网权限处理，以及开始前和每秒解析实际 video transport → selected pair → local/remote candidate 的验证。当时这些改动仍未解决本机 Electron 的 UDP `error 53`；macOS UI 中 Electron 的 Local Network 已开启。2026-10-28 已通过隔离实验将根因定位到端口 9 的 STUN 占位配置，见后续章节；接口枚举与权限并非已证实根因。没有修改系统权限、VPN 或网络保护设置。

补测尝试 `browser-A2-udp-10min`、`browser-A3-udp-10min` 和三轮诊断 smoke 都被 UDP 准入检查拒绝，没有凑入有效长测。当前台架默认要求 UDP；显式 `--transport tcp` 可以做 TCP 对照，原生会在收集 ICE 候选前关闭 UDP。两种模式都要核对实际结果，不能仅凭启动参数称为 UDP 或 TCP。此前三轮运行源码为 `4635153`，原生 TCP 补测为 `e997de7`；后者增加了协议准入及退出错误处理，UI 工作量和解码/绘制管线保持一致。

### 长测结果

同协议 TCP 对照如下，数值范围覆盖两路相机。标准差越小代表解码输出节奏越均匀，不等于屏幕帧间隔。网页 TCP 使用 IPv4，原生 TCP 使用 IPv6 link-local，均经过 Orin 的 `gbe1` 局域网链路，无 TURN；候选身份和地址族仍有差异，不能宣称完全相同的网络条件。

| UI 阶段 | 网页 TCP 解码间隔标准差（ms） | 原生 TCP 解码间隔标准差（ms） | 原生 RTP jitter 输入→GL draw p99（ms） |
|---|---:|---:|---:|
| 空闲 | 7.03–7.09 | 3.19–3.26 | 2.68–2.71 |
| 每 100 ms 工作 8 ms | 7.73–7.78 | 3.07–3.12 | 2.10–2.24 |
| 每 100 ms 工作 40 ms | 7.65–7.78 | 3.13–3.14 | 1.99–2.17 |
| 每约秒停顿 200 ms | 7.67–7.71 | 3.10–3.32 | 2.25–2.26 |
| 恢复空闲 | 7.86–8.12 | 3.16–3.35 | 2.44–2.49 |

两端各阶段解码均为 **29.99–30.01 fps**。网页完整观测期间 RTCStats 新增丢帧、RTP 丢包和冻结计数均为 0；浏览器自身的视频路径也能在本次 JS 主线程停顿下继续工作。本次支持“原生路径的解码节奏更均匀”，没有支持“网页因为 UI 压力严重掉帧，而原生解决了它”。

原生 TCP 完整 600 秒中，两路各出现一次 **104.71 / 108.15 ms** 的 GL draw 间隔，发生在第 **241.82 秒**，位于预先规定的阶段切换排除区间内；阶段表最大值为 91.81 ms。全窗口异常已单独写入证据，不能用阶段表称为没有超过 100 ms 的尾部。两轮原生 UDP 的完整窗口最大 draw 间隔分别为 62.25/67.76 ms、54.33/55.51 ms，无超过 100 ms 的间隔；其阶段解码间隔标准差为 2.77–3.17 ms。这些 draw 时间仍不证明屏幕是否出现等长停顿。

延迟只得到接收端内部证据：原生 TCP jitter 输入→GL draw 的阶段均值 **1.16–1.47 ms**、p99 **1.99–2.71 ms**；网页 `receiveTime→presentationTime` 的阶段均值 **23.38–30.18 ms**、观测 p99 **40.60–45.00 ms**。两者起止点不同，不能相减并声称相机到屏幕减少了 20–30 ms。网页 RVFC 覆盖率 **64%–87%**，压力阶段有漏采样，因此其观测尾部还可能低估未回调帧；呈现计数仍约 29.92–29.97 fps。原生到达时间关联覆盖率均为 100%，未发现负时长。

三轮原生报告的 RTP 唯一时间戳和解码数逐路一致，没有重复/无效解码 PTS 或观察溢出；两路 NSView 均已挂载独立绘制子视图。四轮页面测量期间保持可见、聚焦，远端每秒竞争监控未检出重叠。Electron 进程 CPU 采样均值：网页 TCP 5.02%，原生 TCP 4.77%，两轮原生 UDP 4.85%/5.06%；没有足够证据声称 CPU 大幅降低。原始工作集统计是多个进程相加，可能重复计算共享页，不作为内存节省结论。

本次验收：混合 UI 入口和四轮各 10 分钟采样完成；“同协议、相同额外 UI 负载”的比较已完成，严格相同地址族/硬件解码实现/光学端点的比较仍未完成。继续保留实验入口，下一步应统一呈现测量和实际网络候选，再增加 3D/GPU 负载及弱网场景，而非直接将其设为生产默认播放器。

Mac 构建和 4 组定向 CTest 通过（含 5 项混合分析测试、已有分析测试、真实 Gst PTS 和隔离检查）。实现阶段另通过 104 项控制逻辑测试、桌面测试 33 项通过/6 项跳过。代码 `4635153` 的发布 CI 最终通过，Mac 既有 product-direct suite 首次报 `recovery did not withdraw DC`，重跑通过，未放宽测试或控制门槛；其离线 Linux 全量测试在依赖安装阶段耗尽 25 分钟，不能视为通过。后续代码 `e997de7` 的 Mac、Windows x64/arm64、控制端 Linux、云端 Linux 及离线 Linux 全量检查已通过；车辆端构建在记录时仍运行，以 PR 当前检查为准。

收尾已验证：相机无占用、CAN DOWN、本任务 Orin/Mac 进程退出、18081 监听关闭、临时凭据删除、专用 SSH 隧道关闭。未进行实车或生产部署。

### 已确认的实现与验收边界

- 复用同一接收核心、产品页面、登录、信令与会话围栏；没有复制整套桌面 UI 或重写媒体协议。
- 接收与解码不在网页主线程上运行，解码后采用有界队列，原生绘制不等待 JavaScript 回调。每路使用独立 GL drawable，并检查两块原生视图都真实显示。
- 分别保存解码输出、GL draw、浏览器呈现计数、采样覆盖率、实际 UI 工作量、候选对和帧身份。回调漏采样不会被称为掉帧。
- 这些借鉴了 Sunshine/Moonlight 的硬件解码、减少排队、隔离 UI 与视频路径、分段测量的思路。本次没有移植其协议、FEC、显示刷新调度、Metal 渲染或拥塞控制，也没有证明零拷贝。
- 原生路径仍是显式实验入口，未进入默认桌面发布包；没有驾驶输入、原生视频健康准入、弱网恢复或光学延迟验收。

复核：Reuse 复用了接收器和控制台已有逻辑；Quality 将平台视图适配与接收核心分开，并对协议、会话、队列、样本数量和退出状态作检查；Efficiency 的逐帧数据只在默认关闭的实验路径采集，视图位置未变化时不重复更新。值得在本仓库固定“版本冻结 → 实际连接对准入 → 相同 UI 负载 → 分阶段指标与原始证据”流程，尚未新增独立 skill。


## 2026-10-28 UDP 根因与跨网准入

**跨网 A/B 尚未执行，网页和原生均为 0/1800 秒。** 用户要求控制端与车端处于不同网络；Orin 当前只有 `default via 192.168.2.1 dev gbe1 metric 600`，通过本台 Mac 共享上网。用户确认暂时没有独立公网连接，因此中止局域网 smoke，未以云端信令、同 NAT 回流或 VPN 中继替代真正跨网直连。前述 40 分钟结果全部属于局域网诊断，不是实际应用环境验收。[本轮证据及原始文件哈希](evidence/ipv4-udp-diagnosis-20261028.json)。

根因是实验配置 `stun:127.0.0.1:9`。在实际使用的 Chromium **152.0.7977.78** 中，[P2PSocketUdp::DoSend](https://github.com/chromium/chromium/blob/152.0.7977.78/services/network/p2p/socket_udp.cc#L420) 对受限 STUN 目标端口调用 `OnError()`；[受限端口表](https://github.com/chromium/chromium/blob/152.0.7977.78/net/base/port_util.cc#L39) 包含 9。不能将此配置错误归因于浏览器天生无法 UDP 直连，也不能将此前 TCP 回退造成的差异全部归因于原生渲染。

同一 Electron 44.3.0 下，用两个本机 RTCPeerConnection、只交换 IPv4/UDP 候选做三种配置对照。无 STUN 与回环高端口 34789 均建立连接、在观察阶段接收 51 条数据；端口 9 两端均 failed、接收 0 条。它们是同机数据通道隔离实验，不是视频或跨网性能验证。切换 GStreamer 环境变量与权限处理未解释这一差异。随后 Orin 网页 smoke 确认选中 `192.168.2.1 ↔ 192.168.2.2 / UDP`，但因用户要求真实跨网而立即终止，明确排除。

新增 `--network /absolute/wan.json` 为两组实验共用的严格配置，字段如下；尖括号内容必须替换为当次实测值，示例本身不可运行：

```json
{
  "kind": "wan-ipv4-udp",
  "localIp": "<Mac 实际出口网卡的 IPv4>",
  "localPublicIp": "<Mac 在所用 STUN 服务上观察到的公网 IPv4>",
  "remotePublicIp": "<Orin 在同一 STUN 服务上观察到的公网 IPv4>",
  "stunUrl": "stun:<已解析并确认可达的 STUN 公网 IPv4>:3478"
}
```

该实验入口只接受明确的公网 IPv4 STUN 地址，端口限定 3478 或 19302；不是对产品全部 ICE 配置的限制。台架信令必须向两端公布同一个 STUN URL 且不配置 TURN，否则在开始测量前报错。发送端沿用 PR25 的 ICE 配置能力；原生接收端新增 `webrtcbin.stun-server` 配置，并在候选收集前绑定 Mac 指定地址、关闭 ICE-TCP。

网页和原生使用同一候选过滤与结果检查：接收候选仅保留指定远端公网 IPv4 的 UDP host/srflx/prflx，保留 SDP 会话及 ufrag 围栏；拒绝私网、CGNAT 共享地址、保留地址、IPv6、TCP 和 relay。开始前及每秒检查实际选中候选，原生额外在 libnice 候选对变更回调中锁存任何不匹配错误，避免短暂切换被周期采样漏掉。原生保存两端地址、端口和每秒候选对。公网地址变化会使该轮失效，需要重新记录配置并开始新一轮。

**两个公网候选仍不足以证明独立出口。** 必须同时保留两端物理接口/路由、STUN 映射、连接期间实际选中候选对和出口证明；排除同一共享网络的 NAT hairpin、统一 VPN 出口与中继。分析结果的 `independent_uplinks_verified` 固定为 false，必须用外部路由证据补充验收，不能仅把该字段改为 true。当前没有这种外部条件，原生 STUN 路径也尚未通过真实跨网运行验证。

网络条件具备后的验收顺序：先确认两端独立出口、同一公网 STUN 和公网信令可达，再做两种接收模式短测；确认均为指定 IPv4/UDP 后冻结源码、二进制和台架配置，分别运行 `--seconds 1800 --transport udp --network /absolute/wan.json`。每组五个阶段各 6 分钟，总采样 60 分钟，保留相同 UI 工作量、窗口、视频参数及后台竞争监控。信令也必须经公网可达通道，不能继续使用本轮 Mac→Orin 私网隧道。现阶段没有部署公网台架服务或启动后台长测。

分析脚本新增 P50/P99.9，并同时输出完整窗口及分阶段结果，包括 >100ms/>250ms 间隔。网页 RVFC 漏采样仍需报告覆盖率，观测间隔不等于实际屏幕停顿。用历史原生 TCP 数据回放验证，完整窗口仍检出两路 104.708/108.152ms 的间隔，各一次；没有让阶段边界掩盖尾部。接收端时间点仍不等于光学相机到屏幕延迟。

本轮本地验证：Mac 原生模块和探针构建通过，4 组定向 CTest 通过（混合分析 7 项、原分析 3 项、真实 Gst PTS、隔离检查），桌面测试 37 通过/6 跳过，包含 4 项网络策略回归。未运行新的跨网媒体测试或 30 分钟 A/B，未验证生产控制链路。

独立客户端的收益主要是能直接控制网卡、UDP socket、缓冲和硬解/绘制调度；仅用 Electron 包住相同网页，仍使用 Chromium 媒体路径。换成原生接收栈可减少 UI 主线程对视频路径的干扰，但仍需网络、ICE/STUN 以及适当的中继回退策略。[ICE 标准](https://www.rfc-editor.org/rfc/rfc8445) 描述了使用 STUN/TURN 完成 NAT 穿越的机制；更换 UI 不会消除运营商 NAT 和防火墙限制。当前证据不支持“换客户端就必然更容易公网直连”或“公网延迟大幅降低”的结论。

收尾检查：Orin 两个相机无占用、CAN DOWN，本任务进程和 SSH 隧道已退出，两端本轮临时凭据已删除。上一提交 `c72de2f` 的 macOS CI 在 `mine-teleop-product-direct-integration-tests` 报 `recovery did not withdraw DC`（与较早轮次同一失败信息），其他平台构建及离线 Linux 通过；保留该未解决记录，没有修改控制代码或放宽测试门槛。当前提交的 CI 以 PR 实时状态为准。

## 2026-10-28 双栈、候选证据与有界回退

本节取代上节“仅 IPv4”的新实验配置要求；旧 `wan-ipv4-udp` 仍兼容，保留其
单 STUN、禁止 TURN 的原含义。新增 `wan-dualstack` 允许选择 `ipv4`、`ipv6` 或 `dual`，
明确分开直连 A/B（`allowRelay:false`）和回退可用性实验（`allowRelay:true`）。
同一物理出口、私网直连、IPv6 link-local/ULA、CGNAT 共享地址均不作为公网对端验收。
该限制是实验隔离规则，不是“这些网络绝对不能 P2P”的结论。

示例为结构模板，尖括号值必须替换，不能直接运行：

```json
{
  "kind": "wan-dualstack",
  "family": "ipv6",
  "allowRelay": false,
  "localAddresses": ["<Mac 出口网卡当次公网 IPv6>"],
  "localPublicAddresses": ["<Mac 当次公网 IPv6>"],
  "remotePublicAddresses": ["<Orin 独立出口当次公网 IPv6>"]
}
```

IPv4 组将 family 改为 ipv4，localAddresses 可为网卡私网或 CGNAT 共享地址，另外提供双方实际公网
映射地址。dual 组必须提供两族本地/远端地址；它用于选路观察，不混入固定地址族的
四组性能 A/B。Mac 原生从 offer 取得经过网页同一策略过滤的信令 ICE 列表，复用车端的
GStreamer 配置函数，支持带方括号 IPv6 的服务地址和 TURN 短期凭据。候选及选中对的
类型、UDP、地址族、出口地址均受校验；原生每次选中对变化都锁存违规。

Mac 原生跨网探针可额外指定 `"localInterface": "en0"`：启动前验证所列本地地址属于该接口，
并在 libnice 新候选出现时为 UDP socket 设置 macOS 接口绑定；选中对变化时读取选中 socket
的绑定，验收和分析均要求 `interface_bound:true`。绑定失败会使该轮失效。此设置只用于台架
原生接收器，不更改系统路由或产品运行时；网页 ICE 无法通过此入口强制绑定物理接口，因此指定
该选项的网页实验会被拒绝。接口绑定和候选检查仍不能单独证明两端独立出口：要另行保存信令
与发送端路由证据，持续检查路径变化，并说明采样间隔的局限。

### 2026-10-28 蜂窝出口直连初测

Orin 使用 RM500U 蜂窝出口，Mac 使用独立物理出口；信令经云端 SSH 隧道，媒体不经云端或
TURN。Mac 原生 ICE socket 绑定物理接口，发送端每秒检查到公网对端和信令的路由。具体无线
制式尚未独立读取，不能据此声称已经验证 NR SA 或 NSA。

IPv4 两个 STUN 服务器观测到蜂窝侧不同的映射端口。一次 15 秒双向 UDP 探测双方均未收到
对方报文；真实 WebRTC 短测产出了公网 `srflx/UDP` 候选，但 90 秒内未选出候选对并超时。
这说明当前组合下未建立 IPv4 直连，不能单凭一次尝试判断所有 IPv4 连接不可行或断言 NAT
过滤类型。IPv6 双向 UDP 探测成功；真实双 ISX031 → Mac 原生接收器的 60 秒短测选中公网
IPv6 host/host UDP，采样的直连时间约 60 秒，两路解码约 29.8 fps，解码间隔 p99 约
54–55 ms、最大约 136 ms。这不是相机到屏幕的端到端延迟，也不是网页与原生的对照结果。

首个严格台架配置的时钟同步不确定度约 37 ms，超过 25 ms 门槛，未通过启动；随后仅在隔离
台架副本放宽到 100 ms 以观察媒体，控制仍禁用、mock、零速度且 CAN DOWN。产品配置未改，
这不能证明控制安全或允许使用不可靠的跨端时钟计算端到端延迟。一次长测约 30 秒时检测到
IPv4 默认路由短暂切换到 Mac 共享接口并主动终止，不计入长测验收。

重新启动后的 IPv6 原生长测完成 1800.004 秒，选中公网 host/host UDP，采样到的直连
保持时间为 1800.003 秒，relay 0 秒、候选对变化 0 次；Orin 路由检查每秒进行，未检测到
违规或竞争任务。两路完整窗口解码约 29.788 / 29.745 fps，解码间隔 p95 为 44.56 / 42.15 ms、
p99 为 51.85 / 50.71 ms，最大值约 629 ms。两路 GL draw 间隔超过 100 ms 分别为 125 / 137 次，
超过 250 ms 分别为 10 / 8 次，最长约 630 ms；这些是观察到的长尾，尚未定位成因。
其中约第 723.4 秒的 630 ms 和第 1057.0 秒的 562 ms 间隔在两路近乎同时出现；
相应 RTP marker 的 jitterbuffer 输入时间也已有约 628 ms / 561 ms 间隔。这提示最明显
的停顿发生在解码输出之前，但该时间不是内核收包时间，不能据此区分发送、网络和接收线程调度。

2026-10-29 保持 Mac VPN 运行并使用原生物理接口绑定复测：IPv6 双相机直连 60.010 秒，
两路约 29.96 fps，解码间隔 p99 为 49.22 / 49.96 ms；IPv4 复测在启动期限内仍未选出
候选对。两台 STUN 上的蜂窝侧映射端口依然不同。此轮保留禁用控制的台架配置和每秒路由
监测，未取得网页对照或新的 30 分钟结果。首次 IPv4 重试因检测到其他 GPU 任务而在启动前
退出，该任务结束后才进行复测。[脱敏证据](evidence/5g-direct-recheck-20261029.json)。

该窗口包含空闲、每 100 ms 分别施加 8 ms 和 40 ms UI 工作、约每秒注入 200 ms UI 卡顿、
以及恢复阶段；分阶段统计见[脱敏证据](evidence/5g-direct-20261028.json)。采样范围内
jitterbuffer 输入到 GL draw 的 p99 约 3.25 ms，只覆盖接收端的一段处理，不包含采集、编码、
网络或屏幕扫描。路由每秒采样不能排除更短的变化，也不证明基站切换、移动中表现、TURN
回退或控制恢复。Mac 网页流量默认仍经 VPN，网页对照尚未执行；因此这次不能判断原生 UI
相对于网页是否降低延迟或波动。

要启用产品回退，在测试车辆 YAML 设置 `cloud.ice_direct_timeout_ms: 3000`，信令服务
配置真实 STUN/TURN。具体状态和安全边界见 [ICE 配置说明](08-configuration.md#ice-配置来源与直连回退)。
不修改产品默认码率、FEC、拥塞控制、打洞算法，也没有为追求直连而定期打断健康 relay。
`allowRelay:true` 仅表示实验允许中继，不能把它标成直连成功；严格验收还受下文 TURN leg 证据限制。原生和网页均记录 attempt
的 negotiation ID / route mode；分析器另外输出采样窗口内 direct/relay 保持时长、relay
时间占比、路径变化次数、观察到的 relay→direct 次数。采样缺口超过 3 秒不计入保持时长。
这些是观测值，不证明控制持续可用；连接建立率还需要保留所有成功/失败尝试作为分母，
relay→direct **升级成功率尚不可验收**，因为主动升级策略未实现。

### 对 review 的验证顺序与修正

1. 先检查 `vehicle_ice_configuration` 的配置来源及实际 applied 值，再看两端候选日志。
   车端 YAML 的旧 ICE 键改为显式拒绝，不再误导成生效配置。已有网页 candidate type / family
   信息保留，补地址类别；生产车端补齐 selected-pair-full 证据，不只依赖台架探针。
2. 没有 srflx 时检查 STUN 可达和 UDP，但同时检查是否有可达的公网 IPv6 host，不能直接断言
   跨网不可能。100.64/10 是共享地址段，不是公网映射的必要形态；CGNAT 的 STUN 响应通常也
   可以是公网地址。相同公网地址可能是同出口，也可能有 hairpin，不能单凭它判断 NAT 类型。
3. 比较两个 STUN 服务时，必须复用同一个绑定 UDP socket、相同地址族且目标地址不同，
   否则端口差异可能只是本地重绑。端口变化提示映射依赖目的端；它不单独证明过滤行为或
   “两端无法打洞”。[RFC 5780](https://www.rfc-editor.org/rfc/rfc5780) 规定的行为发现还需服务端配合。
4. 使用独立公网出口，先关闭 TURN 做预算相同的直连对照，再开启 TURN/回退；保留完整
   候选、连接对和尝试时间线。前者成功说明该次直连可行；失败仅表示该次预算内失败，不能
   直接归罪运营商。结合证据才能区分配置、可达性与选路时序。

[ICE 标准](https://www.rfc-editor.org/rfc/rfc8445) 允许 IPv4/IPv6 host、srflx、prflx 和 relay
参与检查。IPv6 可能避开 IPv4 NAT 映射，却不消除防火墙、移动网络换址、拥塞或中继需求。
所以目标是可用路径和可解释的退化，不保证 IPv6 比 IPv4 的延迟/抖动更小。

### 自动化与剩余验收

新增真实 GStreamer/Libnice 集成夹具：只绑定回环 IPv4/IPv6，先验证两族 UDP DataChannel；
再故意不投递直连候选，使策略超时，销毁旧 Peer，配置本地 coturn，只交换 relay 候选，
要求实际选中 TURN/UDP 且收到数据，凭据使用 coturn REST 的带冒号用户名和 Base64 密码，
覆盖 URI 转义后的实际认证。它验证协议栈配置/回退机制，不模拟运营商 NAT，不含
摄像头、UI 绘制、驾驶命令或端到端实车恢复。缺 coturn 时该项明确 skip，不能算通过。
实际车端回调抽取测试另验证 direct 阶段排除 relay、允许 IPv6 host、新代际允许 relay、
拒绝旧代际候选；网页验证刷新过期 TURN 凭据及迟到刷新成功/失败的隔离。
既有控制安全回归保持原门槛。

**该轮结束时真实跨网进度为 IPv4 网页 0/1800、IPv4 原生 0/1800、IPv6 网页 0/1800、IPv6 原生 0/1800。** 后续蜂窝出口结果见上文初测记录。
待 Orin 取得独立公网出口后，先做上述判别实验，再固定地址族及实际路径、相同 UI 五阶段
负载，各跑 30 分钟。TURN 组单独报告连接建立时间、成功率、重连中断和路径占比，不并入
直连延迟样本。保留 p50/p95/p99/p99.9/max、冻结与完整窗口尾部，不用局域网替代。

本轮验证证据、源码及原始日志哈希见 [双栈回退证据](evidence/dualstack-fallback-20261028.json)。
Linux ARM64/GStreamer 1.20.3/libnice 0.1.18 完整构建通过；最终全量 CTest **69/70**，
剩余 TLS 发送时序用例单独复测通过，因此累计 70 项都有通过记录，但不称全量一次全绿。
首轮的时钟/控制路径失败也保留；两个控制路径 suite 在修改前 `b22e6f1` 的同环境对照中
同样未通过，本轮后续通过，尚未定位波动根因。没有放宽控制安全断言；只更新因新增 route
mode 而过时的页面字符串检查，并用实际页面函数测试验证配置行为。补齐 openssl 后完成 TLS 复测。
Mac 原生模块/探针构建和 6 组定向 CTest 通过；网页实际方法 19 项、控制逻辑 104 项、桌面
39 项通过/6 项明确跳过，混合分析器 9 项通过。没有运行本轮 Mac 原生窗口回退实测。

复核：Reuse 复用 ICE 服务配置、地址分类及既有安全关闭/代际围栏；Quality 限制重试次数、
刷新短期凭据并拒绝迟到响应，保留配置迁移与未验收边界；Efficiency 将新增证据限制在 ICE
事件及既有采样周期，不增加产品逐帧追踪。可沉淀为本仓库“独立出口证明 → 候选判别 →
固定族/路径 A/B”流程，已记录方法，未创建新 skill。

### PR26 二次 review：TURN 取证与车端运行循环

`protocol: udp` 只证明候选传输类型，不能证明客户端到 TURN 服务的连接也是 UDP。
[RTCStats 规范](https://www.w3.org/TR/webrtc-stats/#dom-rtcicecandidatestats-relayprotocol)
把这一段放在 `relayProtocol`，并明确 `url` / `relayProtocol` 仅属于本地候选。
因此网页采样与离线分析保留两端的这些字段；任一 relay leg 为 TCP、TLS 或缺失时，
UDP arm 均拒绝通过。不会从 TURN 配置的 `?transport=udp` 推断实际传输。

允许 relay 的 WAN 配置现在还必须提供显式 TURN URL 和分配地址池，例如在上述配置中增加：

```json
{
  "allowRelay": true,
  "turnServers": [{
    "url": "turn:turn.example.com:3478?transport=udp",
    "relayAddresses": ["<本次 TURN 分配池的公网 IPv4 或 IPv6>"]
  }]
}
```

启动时主进程解析 URL 的主机，拒绝空结果或非公网结果，把实际 DNS 快照保存为
`turnServers[].serverAddresses`，并写入带时间的 `turn_allowlist_resolved` 事件。
监听地址和 relay 分配池可能不同，不能用 DNS 地址代替运营者确认的分配池。
本轮信令下发的 TURN URL 必须位于允许列表；每个选中 relay 端须同时满足实际 URL、
`relayProtocol: udp` 和分配池检查。非 relay 一端仍须匹配指定控制端/车端地址，
不会因为另一端是 relay 而放开。每秒样本保留当时信令 URL 列表，分析器再次核验。
迟到的旧凭据响应也受会话、代际及 negotiation ID 围栏约束，不覆盖新 Peer 的白名单证据。
这些证据补充既有认证会话/negotiation 围栏，不证明物理出口独立或运营商 NAT 类型。

**能力限制会明确失败，不再产生假阳性：**浏览器收到远端 relay 时，标准 RTCStats
不给出其 TURN leg，当前尚无经认证的对端取证通道，因而不能通过严格 UDP relay 验收。
原生接收器目前使用的 libnice selected-pair 回调也不能完整证明两端 TURN leg；保存为
`unknown` 并锁存失败。`allowRelay:true` 允许尝试连接，不能覆盖证据缺失。
本轮没有实现这条跨端取证通道；之后需要把对端证据与相同 session、negotiation、候选对
绑定，才能放开相应中继 arm。生产 TURN 回退仍可使用，不受台架验收限制影响。

新增 `mine-teleop-vehicle-ice-fallback-runtime-tests` 直接编译并运行完整
`VehicleMediaRuntime::run()`，连接回环认证信令服务和真实 GStreamer answerer：

- matching answer 成功应用后不投递远端候选，由生产循环的直连预算触发回退。
- 检查刷新 TURN 凭据前，控制服务、旧 channel 和旧 answer 状态已被关闭/撤销；
  新 Peer、negotiation ID 和凭据均不同，旧 readiness 不继承。
- 在替换 Peer 尚无 answer 时注入旧 promise completion、candidate、DataChannel 回调，
  并把迟到的 answer/candidate 送入真正的车端 HTTP polling 路径，检查队列、ufrag、
  answer 状态和控制状态不受污染；新 mock 服务建立后，旧 channel 仍不能关闭它。
- 在第二次凭据请求期间发送 shutdown，检查不再生成新 offer；生产代码在刷新前后
  增加 stop-token 检查。超过首轮预算继续观察，确认没有第三次重建。

此夹具严格使用 `test_bench_*`、mock 适配器、无相机/录像/上传、无 CAN。
为确定性检查 DataChannel 生命周期，测试调用真实 on-open 回调建立 mock 服务前置状态；
测试屏蔽车端云控制 WSS，避免另一授权路径并发启动服务，控制台信令仍正常工作。
没有修改产品安全门槛，也没有模拟 VCU 已就绪。其 TURN 地址故意不可达，仅验证生产
编排和凭据替换；真实 coturn 分配与 DataChannel 收发仍由另一个组件夹具验证。
**真实路径故障后的完整车辆控制重新就绪、TURN 质量、原生 UI 运行验收仍未证明。**

同时修复 `fe80::1%en0` 诊断分类：仅解析时剥离 zone，产品候选日志仍只有地址族和类别，
不输出原始 IP、接口名或 zone。C++ 和 JS 都有隐私负例。

本轮结果与日志哈希见 [review 修复证据](evidence/pr26-review-20261028.json)。
`cloud.ice_direct_timeout_ms` 默认仍为 **0**，现场配置未启用 direct-first；
没有健康 relay → direct 的周期性升级。四组独立公网 A/B 仍各 **0/1800 秒**，
本轮未连接 Orin，也没有新增 LAN A/B。

### Review 补充：回退验收、真实编解码与恢复证据

`allowRelay:true` 现在仍只是路径许可。普通媒体实验使用默认的
`validationGoal:"media"`；若要验收回退，必须在 WAN 配置中显式设置
`validationGoal:"fallback"`，并保留前述 TURN URL、DNS 和分配池证据。
分析器要求同一 session/control generation 中有 `direct → all` 两次尝试，
negotiation ID 不同；测量样本必须属于新协商。还必须在相邻有效样本中看到
同一合法 relay 选中对，且该对的接收字节、对应视频流的接收字节及实际解码帧数均增长。
全程 direct、旧协商、未知/TCP/TLS relay leg、仅队列出帧但接收计数不增长均不能通过。
采集程序结束不等于验收通过，必须运行 `hybrid_receiver_analyze.py`。

**网页证据仍是每秒采样，不是连续路径合规证明。** `path_validation` 明确记录此限制；
两个样本之间的瞬时路径变化可能漏掉。此次没有通过新增高频轮询改变 UI 压力实验。
原生保持 selected-pair 回调锁存违规，但完整 TURN leg 取证仍缺失，不能通过严格 relay arm。

本轮采用的机制参考了 Sunshine 的实际编码/首包 IDR 检查
（[`validate_config`](https://github.com/LizardByte/Sunshine/blob/master/src/video.cpp)）以及
Moonlight 的实际测试帧解码与渲染路径试验
（[`ffmpeg.cpp`](https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/video/ffmpeg.cpp)）。
以下为本项目已落地的子集，不表示已复现上游完整恢复与显示架构。

| 项目 | 本轮实现 | 仍未验证或未实现 |
| --- | --- | --- |
| 真实能力 | 独立进程运行生产编码器配置，输入 16 帧 NV12，检查首 AU 的 IDR/参数集，实际解码首帧并核对尺寸、格式、PTS | 输入是合成帧；未接入真实相机启动验收、生产自动后端选择或实际 GL 显示验收 |
| 参数与内部排队 | 读回实际 GObject 属性；以 PTS 关联编码器 sink 到解析 AU 输出，记录分布及同时等待输出的输入数 | 属性读回不保证驱动内部实现；16 帧不足以证明长时性能，不能代替各硬件/分辨率实测 |
| IDR 恢复 | 复用生产 freshness probe，拒绝旧 IDR 解开屏障；每次故障最多一次补发，记录恢复时间和超时；实际 ForceKeyUnit→新 IDR→匹配 PTS 的解码输出测试 | 发送端指标不是远端解码 ACK；没有新增跨端恢复确认、原生解码器自动重建或车辆控制重新就绪证明 |
| 缓冲预算 | 报告已配置队列槽位、实际解码内存类型；原生丢弃 CORRUPTED buffer | 驱动引用 surface、GPU 在途占用和池容量未知，`verified:false`；未调整 surface、async-depth、VSync，也未宣称 zero-copy |
| 阶段统计 | 产品采集→编码输出、请求→发送 IDR 增加 p50/p95/p99/max；复用原生 jitter 输入/解码/GL draw 的逐帧 PTS 关联 | 尚无贯穿相机、RTP、两端时钟和扫描显示的统一身份链，不能把不同边界的耗时直接相减 |

启用 `MINE_TELEOP_BUILD_BENCH_PROBES=ON` 后构建 `mine-teleop-codec-roundtrip-probe`，
通过监督脚本运行（示例为软件基线；输出文件不可覆盖）：

```sh
python3 scripts/test/codec_roundtrip_probe.py \
  --probe build/mine-teleop-codec-roundtrip-probe \
  --encoders cpu --decoders avdec_h264 --codec h264 \
  --width 128 --height 96 --fps 20 --bitrate-kbps 125 --gop 30 \
  --timeout-ms 10000 --output codec-roundtrip.json
```

按明确的顺序传 `--encoders nvenc,vaapi,cpu` 可测试失败后选择后端，
每个组合使用新的子进程，保留每次失败原因，首次实际成功后停止。CPU 只支持 H.264；
NVENC/VAAPI 可以分别测 H.264/H.265。`nvv4l2decoder` 可作为解码候选，仓库尚无
nvv4l2 编码实现，不能将 NVENC 参数当成该实现的证明。监督脚本限制整次尝试的时间，
包括驱动初始化和 teardown；超时杀进程组，未回收成功则禁止继续选择下一后端。
这不是内核/驱动死锁恢复保证。不要直接运行无外部期限的 worker 作为生产启动过程。

Linux 软件样例确认 x264 的 `bframes=0`、`rc-lookahead=0`、`sync-lookahead=0` 和
zerolatency tune 被读回；默认 `vbv-buf-capacity=600` ms，在 20 fps 下为 12 帧码率预算。
该样例 16 帧最多只有 1 个已输入但未输出的编码帧，说明 VBV 预算不能直接解释成排队时长。
目前未强制缩小 VBV，需在目标码率、复杂运动、画质与吞吐约束下做后端对照。
直方图使用 1 ms 精度（0 表示不足 1 ms），超过 2000 ms 的尾部计数保留、对应分位数
输出 null，max 保留；不会把超范围尾部截成 2000 ms。运行中快照是并发计数，停止后精确。

生产发送恢复在持续收到 AU 时，以现有 freshness 年龄预算触发一次补发，两个预算后记录
一次超时；持续丢失的依赖帧不会引发无限请求。没有新 buffer 时仍由既有视频 watchdog
负责停控。晚到但完整、新鲜且身份严格更新的 IDR 可恢复发送资格，不能单独恢复控制授权。
800 ms 保护、5 s 升级和既有会话/控制重启围栏没有放宽。测试的解码确认只来自软件夹具，
未伪装成生产控制端 ACK。保持不使用重复/static frame 更新健康状态、不用损坏画面恢复准入。

本轮自动化结果见 [补充验证证据](evidence/pr26-codec-recovery-20261028.json)。
真实相机、NVENC/VAAPI/nvv4l2 硬件、GPU surface→显示、弱网后跨端恢复和四组独立公网
30 分钟 A/B 当时仍待验证；该轮结束时 WAN 仍各 **0/1800 秒**。该轮没有连接 Orin 或新增 LAN A/B；后续蜂窝出口初测见上文。

后续 CI 发现车辆打包镜像只有 GStreamer NICE 插件，缺少车端直接引用的
`libnice-dev` / `nice.pc`，导致 configure 失败。三个构建入口均补齐开发依赖，
控制平面镜像同时声明运行时库与插件；完整车辆包验收检查程序和 NICE 插件
均解析到包内同一份 libnice，并检查 `nicesrc` 可加载。隔离 Linux 容器的 SDK、
车端程序编译和同库解析夹具通过；完整 x86 包和底盘运行时以 CI 构建为准。

macOS CI 的既有产品直连集成测试出现过路径切换及恢复失败，前一提交也有失败；
本机同一用例连续四次通过，不能据此排除并发或安全恢复缺陷。没有通过重试、
延长 800 ms 门槛或跳过断言来把 CI 标绿。复测结果及未解决的失败记录见
[打包和 CI 证据](evidence/pr26-packaging-ci-20261028.json)。
