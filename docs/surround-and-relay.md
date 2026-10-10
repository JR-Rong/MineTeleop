# 环视、两路传输与中转准入

本实现以 `fb271156b117779402a9b945a3982c457432c248` 为基线。源码、Docker 软件管线和合成样本测试不等于目标车端、Windows 参考机、CAN 或实车验收。未取得有效验收文件时继续完整模式直连平铺；两路驾驶必须通过下面的准入。

## 媒体路线和带宽估算

完整模式持续采集并分别编码六路，浏览器可独立放大每只相机；通过完整模式性能验收后，四路鱼眼由 Worker/CPU WASM 合成鸟瞰。两路模式持续采集六路，在车端进行逐输入检查、时间对齐、投影与融合，仅运行两个输出编码器：

```text
前视 + 后视 → 上下分区 drive_mosaic → H.264 → 前视、后视两个可调整区域
四只鱼眼 → omnidir 地面投影 surround_bev → H.264 → 一个可调整鸟瞰区域
```

鸟瞰不能还原完整鱼眼视野，查看独立鱼眼要先停车并切换完整模式。既有独立 3D 区保留。

| 档位 | 行车合成 | 鸟瞰 | 初始编码预算 |
| --- | --- | --- | --- |
| 720p | 1280×1440@30，每区1280×720 | 512×896@20 | 2.8+1.2=4 Mbps |
| 540p | 960×1080@30，每区960×540 | 384×672@20 | 1.4+0.8=2.2 Mbps |

鸟瞰画布按实测地面范围等比例投影，比例不同时留边，不裁掉覆盖范围、不拉伸。有效地面区域由共享映射产生的 mask 单独报告。

六路各1.5 Mbps为名义9 Mbps。行车合成加鸟瞰的工程目标3.2–4.5 Mbps，预计降低50%–64%；行车合成新增约5–30 ms、CPU鸟瞰约20–80 ms。这些是**估算**。保留全部源像素的两路分区合成从同样9 Mbps起测，不承诺省三分之二。540p行车像素减少43.75%。按25%开销，2.2 Mbps视频约2.75 Mbps出口；两次公网发送则约5.5 Mbps。

实际启用的是上述固定码率，不能仅凭估算降低到未经独立画质验收的码率。持续超预算停止驾驶资格；停车后可选择另一个已验收档位。分辨率和模式不在驾驶中切换。

## 第一阶段：编码、SDP、时间戳和基线

先在**目标车端**运行：

```sh
mine-teleop media-profile-probe --config /etc/mine-teleop/vehicle-agent.yaml --backend nvenc --out /tmp/encoder-canvases.json
```

记录 CPU、内核/架构、GPU驱动、实际编码器工厂、每个采集驱动和 GStreamer 版本。实际环境必须与验收文件一致。`--software-fixture` 仅测试x264软件路径，永远返回 `driving_qualified=false`，不能用于车端资格。

1280×1440@30至少H.264 Level4.0；保留四个720p的诊断四宫格2560×1440@30至少Level5.0；540p行车合成至少Level3.2。诊断四宫格不是正式输出。编码caps显式约束等级，等待实际编码SPS/RTP caps后才生成offer，并逐视频m-line验证answer接收等级。普通720p的SDP不能用来证明大画布可解码。能力不足拒绝两路；auto初始化失败可退回完整预览，驾驶已启动后的重建仍需车端停车确认。

控制端基线：i5-12400、16 GB、Windows11、Electron44.3.0，记录OS构建号和实际Chromium版本。禁用GPU与硬件视频解码，并通过实际decoder stats/实现确认软件解码，不能只看启动参数。固定H.264、B帧0、相同编码后端及GOP。目标车端规格由探测结果填入报告，禁止拿Docker主机替代。

先核查V4L2 buffer的sequence、MONOTONIC/时钟域、SOE/EOF标志、读取开始/结束时间和曝光同步能力。SOE可信时采用曝光年龄；EOF只称EOF，未知时采用读取完成年龄并要求外部运动/闪光试验证明软件对齐资格。读取时间不视为曝光时间。

20 ms是对齐目标，不是未经测量的准入上限。四个自由运行30 FPS输入相位均匀错开时最小跨度可能25 ms。`max_skew_ms: 0`从验收报告取得测得的硬上限（最多40 ms）；可配置更严格值，不得放宽超过验收。年龄最多100 ms，等待使用验收值且最多20 ms。原生每只输入只保留最近两帧，最多检查16组；不等待无限队列。没有已确认的时间戳/外部同步资格，不启用鸟瞰两路驾驶。

## 标定工具和场地

独立工具 `mine-teleop-calibrate board/capture/solve/validate` 使用OpenCV omnidir。维护环境安装 `tools/calibration/requirements.txt`（OpenCV4.13.0.92）；驾驶运行时不依赖Python/OpenCV。先停稳、退出车辆控制运行时，再捕获；全局车辆维护锁和逐设备锁防止与运行时争用，`--vehicle-stopped`是现场停稳的确认，不能代替实车停车措施。

```sh
mine-teleop-calibrate board --length 6 --width 2.8 --vehicle-id JYR010 --out /tmp/field-template
# 测量并编辑 field.json，填入实测四角、方向、32个独立检查点、测量精度和平整度。
mine-teleop-calibrate board --length 6 --width 2.8 --vehicle-id JYR010 --survey /tmp/survey.json --out /tmp/measured-field
mine-teleop-calibrate capture --config /etc/mine-teleop/vehicle-agent.yaml --vehicle-id JYR010 --camera fish_front --role intrinsic --samples 20 --vehicle-stopped --out /tmp/calibration-data
# 四只镜头分别拍摄 intrinsic、extrinsic、check。
mine-teleop-calibrate solve --dataset /tmp/calibration-data --field /tmp/measured-field/field.json --out /tmp/draft.json
mine-teleop-calibrate validate --calibration /tmp/draft.json --field /tmp/measured-field/field.json --checks /tmp/calibration-data/checks.json --dynamic /tmp/dynamic.json --performance /tmp/performance.json --report /tmp/acceptance.json --out /tmp/accepted.json
```

维护采集与驾驶运行通过 `/run/lock/mine-teleop-cameras` 互斥，避免 systemd PrivateTmp 隔离使锁失效；运行账户须具备该目录和相机的权限。自定义 `MINE_TELEOP_CAMERA_LOCK_DIR` 时两端必须使用同一共享目录。

生成带尺寸PNG/SVG、八块不同板号的7×5 ChArUco板（格边300 mm，板2.1×1.5 m）、独立检查标记和四角坐标CSV。默认场地长宽比车辆各多6 m，板中心初始在对应车身边界外1.2 m。示例6×2.8 m车辆见 [示意图](surround-field-example/field.png)、[SVG](surround-field-example/field.svg)、[坐标表](surround-field-example/coordinates.csv)。这是**未测量模板**，不能作为施工验收图。现场根据视野调整、重新测量后生成施工资料。图像显示的板原点、方向以坐标表为准。

A1/A3/A6/A8是角部共视板，A2/A4/A5/A7为前/左/右/后。拍完求解图后可撤走板，再放置独立检查标记；检查点不使用求解角点。每镜头≥20有效内参样本，覆盖中心/边缘/不同倾角，RMS≤1.5 px；≥3块分布不同地面板，四个角板相邻相机共视；退化失败。

参数：`K=[fx,s,cx;0,fy,cy;0,0,1]`以原始标定像素为单位，xi标量、D=[k1,k2,p1,p2]。`T_vehicle_from_camera`方向为相机到车辆，米；车辆地面中心为原点，+X前/+Y左/+Z上，相机沿OpenCV右/下/前。地面投影用逆变换。传感器原始尺寸、裁剪、旋转、镜像、缩放按“裁剪→方向→缩放”及像素中心生成A；不同时缩放K。相机身份、设备/安装ID/尺寸指纹、survey哈希、覆盖、时间戳和验收均绑定内容哈希。安装或运行变换变化后重新标定。当前车端采集交付原始像素，拒绝声明了裁剪、旋转、镜像的文件；共享核心支持坐标变换，完整模式的编码缩放由车端明确写入控制端描述。

静态验收固定≥32个未用于求解的点，前后左右和四角八区，每区不同距离，重点车身外2 m和接缝。测量精度/地面平整度≤5 mm，最近秩P95≤5 cm、最大≤10 cm；固定点不能删除，盲区单列。动态四条接缝各以0.5/1 m/s测试≥100点误差，P95≤10 cm、丢失/重复为0。1 m/s下20 ms约2 cm、100 ms约10 cm；高速显示不承诺厘米精度。立体目标的地面投影畸变单独测试。失败报告保留旧有效标定。

## 三层健康和恢复协议

安全检查分别验证六只原始输入（有效时间、序号推进、采集错误/配置）、实际参与输出的每只源帧（年龄/时差/消费推进）、以及该组合对应的编码帧推进。编码probe通过实际PTS匹配输入身份，其他相机更新或重复图不会替冻结输入续健康。帧超龄时立即遮罩，遮罩帧不算健康；超时沿用 `SoftwareFault/CriticalCameraFailed` 停车、解除控制、同云端会话锁止。未解锁启动宽限15 s，驾驶期间无宽限。像素长期相同仅诊断，不把静止场景当故障。

`control_epoch`由车端持有，重启使用随机48位新值，解除控制推进。普通命令、浏览器intent、原生采样、会话profile、VCU握手和全部确认/状态携带epoch；媒体相关消息另带 `media_attempt_id`。旧epoch/旧连接/迟到确认拒绝。原生发送器清空保留输入并暂停，绝不把旧非零输入换epoch；操作者回到N挡/中立后，再产生新的输入。浏览器按键要求释放重按，手柄要求实际中立。重启还需新媒体协商和完整握手。

媒体重试/模式切换走独立WSS `media_quiesce`。车端拒绝旧输入、清空配置、解除握手，依据新鲜（≤200 ms）有效速度≤0.1 m/s、停车反馈与握手解除反馈确认。页面显示停车不构成确认。失去/迟到确认不恢复资格；10 s车端超时、12 s页面超时后只能重新申请。相机故障同会话始终锁止，修复只恢复预览，必须结束旧会话进入新云端会话并重新握手。急停/其他故障沿用原复位条件。

媒体故障立即停止采集/驾驶资格，但保留独立停车WSS和停车反馈周期；只有最终会话/媒体对象销毁才关闭这些线程。普通HTTP传输失败保留当前媒体代次，恢复信令后申请停车确认；停车服务不得重新接受普通驾驶/profile/握手命令，已有急停和故障锁止仍优先。不可恢复的会话鉴权错误按原流程结束会话。

默认ICE all，按RFC候选优先级尝试直连，允许已批准中转。没有默认5 s重建，也没有驾驶期间周期重建。记录SDP实际完成/失败、候选排队/应用/拒绝/结束标记、ICE/DTLS/首帧/VCU分阶段结果。浏览器按transport.selectedCandidatePairId、车端GStreamer1.28按实际selected pair及变化通知识别路径；诊断关联车辆、会话、媒体代次、控制代次与租约，不记录凭证。

## 中转闭环和维护升级

10 Mbps出口中媒体上限8 Mbps，控制/信令/余量2 Mbps。预算含25%初始协议开销和保守两次公网发送；未验证出口拓扑不能承诺并发车辆数。默认540p额度5.5 Mbps；720p额度10 Mbps被拒绝，直到实测并配置出口副本数。无预算仍可STUN直连，relay驾驶拒绝。

顺序：事务预留→车端暖采集/合成/编码（valve阻断RTP、不分配TURN）→设备鉴权确认实际应用码率与健康编码→管理进程将该租约独立密钥加入SQLite并实测Allocate成功→服务器确认当前有效租约再给双方发凭证→ICE→媒体健康及VCU握手。`ice_servers`不再凭普通会话鉴权无条件发TURN。用户名格式 `expiry:realm:lease_id:actor`，账本记双方最近用户名，管理进程按租约枚举其所有用户名及重复allocation。

棘轮式状态 reserved→confirmed→active→revoking→released；迟到续租和撤销在同一账本锁下串行，revoking不可复活。5 s续租、15 s本地资格失联到期：**当前relay立即拒绝驾驶并启动原停车流程**，不等待allocation取消；已确认直连不受备用中转回收影响，但失去直连后不能回旧relay，需停车再准入。实际selected pair变化同时关闭非法relay上的旧输入资格，独立watchdog执行停车。

凭证600 s仅用于鉴权，不代表allocation回收。撤销先删SQLite独立密钥，再CLI按session ID异步取消全部用户名的allocation，实测旧凭证Allocate被拒，连续5 s无残留后释放预算。两端实际选中直连、控制端所有输出持续解码显示且DataChannel健康10 s只是启动回收条件；控制端每秒回报帧推进，任何100 ms显示间断或超时回报都会重置等待。未知管理状态、回收失败、重启未对账保留额度并停止新准入。账本历史4096项后拒绝新relay并要求维护归档，禁止在有allocation/驾驶时删除账本。

固定系统coturn **4.6.1 / SQLite**（Ubuntu24.04测试包4.6.1-1build4），不是master源码承诺；实际CLI/动态数据库/取消测试另见 `scripts/test/relay_manager_test.py`。管理CLI仅127.0.0.1鉴权。每秒读取allocation与瓶颈媒体class出口，5 s窗口核对；coturn `bps-capacity/max-bps`（字节每秒）是次级保护，会丢包，不能替代准入。

CLI速率字段按[4.6.1 的实现](https://github.com/coturn/coturn/blob/4.6.1/src/apps/relay/turn_admin_server.c#L506)以字节每秒读取，再换算 bit/s；`cs`发出异步取消请求，必须枚举确认消失。公网计量采用实际媒体叶队列 `tc -j -s qdisc` 的发送字节（部分 iproute2 的 HTB class 输出不是 JSON），不能用用户名数量代替出口流量。

部署脚本安装独立coturn身份、root私有账本/管理进程、出口整形systemd服务。**仅维护窗口升级**：停驾驶及旧cloud target/coturn→备份数据库/保留账本→清理旧realm密钥和旧allocation→设置环境中公网瓶颈 `MINE_TELEOP_RELAY_INTERFACE`→同步升级三端→启动对账。新版部署需要Ubuntu24.04、coturn4.6.1，旧静态REST密钥不再提供新凭证。CLI密码文件是本机管理密码，不是共享TURN REST密钥。

整形脚本只在配置的公网接口上建立10/2/8 Mbps HTB。coturn专用UID打媒体mark0x20；HAProxy对共享443/6000明文TURN内容设置mark，WSS走控制优先级，不能按共享端口直接限速。整形替换该接口qdisc，部署前核对现有网络策略；未正确配置/无法计数时中转停止准入。没有在本次开发中部署或修改公网接口。

## 可复现实验与报告

隔离台架可使用 `mine-teleop vehicle-media-agent --config BENCH.yaml --codec h264 --diagnostic-partition --service` 生成上下行车及四宫格鱼眼两路（3+6=9 Mbps）。此 CLI 限定 bench、强制关闭控制，描述标记 `driving_available=false`；鱼眼2560×1440需要Level5，不是驾驶模式或鸟瞰。退出该诊断运行后才能重新进入普通模式。

比较六路独立、保留全部720p像素的两路分区诊断、行车+鸟瞰720p/540p。同一输入/编码后端/GOP/H.264/B帧0，预热30 s，每次600 s、三次，覆盖静态/移动/复杂纹理。完整模式六路1280×720@30不降源输入；车端/云出口、编码字节、CPU/RSS、掉帧、同步/转换/合成/排队/编码/网络/解码/显示均采集。`tools/benchmark/compare.py`分析计数器和按相同**光学测试帧ID**配对的延迟差，不能两组P95相减。光学不确定度>5 ms样本不进硬门槛。

开启 `runtime.media_frame_trace: true` 后原始身份为媒体attempt/相机/generation/64位扩展sequence；相机重开推进generation，V4L2回绕扩展sequence，旧generation/重复/逆序不续健康。合成源列表→实际encoder running PTS（处理x264 segment偏移）→RTP marker timestamp/SSRC/seq→浏览器RVFC.rtpTimestamp关联；缺失/掉帧保留unmatched，旁路消息迟到以原显示时间补关联，禁止最近帧猜配。关联缓存256、编码输入映射32、RTP映射64均有上限。没有RTP timestamp的浏览器帧不能算精确关联。VideoFrame本身不含自定义源ID，Worker消息显式带源rtpTimestamp。跨机器monotonic值不能直接相减；浏览器captureTime只是估计。

`tools/benchmark/collect-browser.mjs`在隔离台架收集帧事件，队列1 MiB溢出计丢失；Worker忙时每源最多一个最新待处理VideoFrame，覆盖立即close，最多一组四帧在执行。输出及copy/compose阶段另发关联事件，重连终止Worker并释放所有待处理资源。

性能验收：鸟瞰有效≥20 FPS且≥384×672；有效覆盖不缩水；逐帧配对新增端到端P95≤50 ms；行车同分辨率ROI SSIM损失≤0.02；鸟瞰有效地面mask（排除车身/空白，11×11窗口侵蚀）SSIM≥0.95。画质还需 `tools/calibration/quality.py` 全部252个用例：日间/夜间/扬尘×静止/1 m/s；前后人0.5×1.6 m/10–30 m、锥桶0.3×0.5 m/5–20 m、柱0.1×1 m/5–15 m；四接缝0.2×0.2 m障碍和人员/0.5–2 m。源Michelson对比度≥0.1、目标最短≥4像素、可见≥80%、丢失/重复0、三位盲评观察者检出≥95%、误报≤5%、位置P95静态≤5 cm/动态≤10 cm。保存源/解码片段哈希；540p独立报告。遮罩/背景不能替小目标放行。

验收JSON结构可从validate代码核对：`phase_one`含编码/SDP/软件解码三项及offer/answer哈希；`vehicle_environment`来自车端probe；`timing`含clock_domain/age_basis/hard_max_skew_ms/max_frame_age_ms/alignment_wait_ms/external_alignment_verified；控制端cpu/ram_gb/os/os_build/electron/chromium；每profiles.full/720p/540p含source_resolution/source_fps/runs/seconds_per_run/warmup_seconds/width/height/bev_fps/paired_extra_latency_p95_ms/optical_uncertainty_ms/drive_roi_ssim_drop/bev_ssim/visibility_cases_passed/software_decoder_verified/coverage_verified/codec/b_frames/quality_report。缺字段、旧报告、环境/几何哈希变化均不准入。

## 录像迁移

不再运行持续录制、分片、录像索引或视频uploader，保留CAN、控制审计、诊断日志收集和标定照片。旧recording/upload/record_profiles/camera.record_profile配置接受但发废弃提示；旧录像命令返回移除说明，已有录像文件不删除。日志包上传继续可用。

## 开发验证与交付边界

在隔离 ARM64 Docker 环境使用生产 Dockerfile 构建的 **GStreamer 1.28.5**：27/27 CTest通过，包括原生控制、VCU/底盘 ABI、信令隔离、媒体健康/epoch/租约逻辑，以及真实 x264 SPS→webrtcbin SDP→OpenH264 软件解码的 Level4/5 夹具。软件编码插件来自测试发行版，不能替代目标 NVENC/VAAPI 验收。新增CI固定使用相同生产GStreamer；发行版1.24的该夹具曾超时，不纳入通过证据。

维护环境 OpenCV-contrib4.13.0.92/NumPy2.2.6/scipy1.15.3：编号板识别、独立投影逆变换、252项画质规则、失败保留旧文件及逐帧光学配对分析夹具通过；实际内外参求解用数学生成角点执行，重复照片或混合安装身份拒绝。维护采集用合成相机验证共享锁、原始时间标记和旧录像配置兼容。没有用合成数据生成驾驶资格。

系统 coturn4.6.1/SQLite 实测新密钥激活、双方用户名与重复allocation、异步取消、旧凭证拒绝、连续5秒为空和管理进程重启；NET_ADMIN测试只操作 `--network none` 容器的lo接口。HAProxy2.8配置检查通过。浏览器渲染实际原生控制台，合成轨道验证四角拖动/双击/保存布局、损坏保存值、真实VideoFrame→Worker→CPU/WASM鸟瞰，页面异常为0。共享WASM由Emscripten重建并逐字节核对。

复现：先构建 `deployments/cpp/Dockerfile.build --target gstreamer` 为 `mine-teleop-gst128:local`，再构建 `scripts/test/Dockerfile.features`；容器内运行 `scripts/test/check_surround_features.sh`。中转和浏览器完整命令见 `.github/workflows/surround-features.yml`。维护依赖保持PyPI哈希检查，截断下载只能重新获取，不能跳过校验。

仍需目标车端编码硬件/驱动及实际SDP、可信曝光/同步、Windows参考机无GPU软件解码、600秒×3场景带宽/延迟、252项实际驾驶画质（540p独立）、隔离CAN台架、现场静动态标定和各网络组50次连接验证。当前收益和新增延迟仍是估算；本次未部署、未调整公网出口、未接入实车。未经这些验收的两路驾驶profile保持禁止准入。
