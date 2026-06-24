# Mine Teleop

矿车遥操系统项目文档。

本目录用于沉淀车端工控机、远端模拟驾驶器、云端信令/中继/上传服务之间的需求、设计、实现计划和验证方案。当前文档来自前期需求讨论和工控机硬件编码排查结果。

## 当前结论

- 场景：封闭矿区矿车，最高车速不超过 40 km/h。
- 部署：车端为 Ubuntu 工控机，驾驶端优先 Windows，也可能 Linux。
- 网络：车端和驾驶端不在同一局域网，云服务器有固定 IP，可以新增支持 UDP 的节点。
- 视频：默认 4 路相机，数量可配置；实时流首版目标 720p 30 fps，可配置分辨率、帧率和码率。
- 控制：驾驶端发送档位、转向、油门、刹车；首版控制输入使用键盘/软件控件。
- 控制频率：首版 20 Hz。
- 安全：控制链路超时后车端执行安全停车。
- 录像：车端按相机采集原分辨率编码保存，分段后批量压缩/上传云端。
- 编码硬件：当前工控机无 NVIDIA；Intel Alder Lake-S GT1 核显存在，Docker 中已验证 `h264_vaapi` 可编码 1280x720 30 fps。
- 推荐主线：C++/Qt + GStreamer/WebRTC；媒体编码优先 Intel VAAPI/QSV，CPU x264 兜底；云端提供 HTTPS/WebSocket 信令和 STUN/TURN 兜底。

## 文档导航

- [项目背景与决策](docs/00-project-context.md)
- [需求规格](docs/01-requirements.md)
- [系统架构](docs/02-system-architecture.md)
- [车端 Agent 设计](docs/03-vehicle-agent-design.md)
- [驾驶端 Console 设计](docs/04-driver-console-design.md)
- [云端服务设计](docs/05-cloud-services-design.md)
- [视频、录像与上传](docs/06-media-recording-upload.md)
- [控制协议与安全停车](docs/07-control-and-safety.md)
- [配置体系](docs/08-configuration.md)
- [硬件与环境排查结论](docs/09-hardware-and-environment.md)
- [实施计划](docs/10-implementation-plan.md)
- [测试与验收](docs/11-testing-and-validation.md)
- [运维与排障](docs/12-operations-and-troubleshooting.md)
- [待确认问题](docs/13-open-questions.md)
- ADR:
  - [0001: 传输与媒体栈选择](docs/adr/0001-transport-and-media-stack.md)
  - [0002: 硬件编码策略](docs/adr/0002-hardware-encoding-strategy.md)

## 推荐第一阶段目标

第一阶段不直接做完整平台，而是做可验证闭环：

1. 车端用 1 路测试源或真实相机采集视频。
2. 车端使用 Intel VAAPI H.264 编码。
3. 通过云端信令建立 WebRTC 连接。
4. 驾驶端显示实时视频。
5. 驾驶端以 20 Hz 发送控制命令。
6. 车端接收控制命令并输出到 Mock Vehicle Adapter。
7. 控制断开或心跳超时时进入安全停车状态。
8. 车端分段保存视频文件，并模拟上传。

完成这个闭环后，再扩展到 4 路相机、真实车辆控制接口、录像批量上传、权限管理和运维监控。

