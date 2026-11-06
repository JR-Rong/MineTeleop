# PR #26 证据索引

每个文件一行：内容、记录日期（取自文件名或正文）、大小、是否仍然适用。
"已失效"和"部分失效"指文件里的比较依赖已经从代码里删除的 DC/WSS 镜像或紧凑 CBOR 格式（见各文件开头的说明），记录本身没有删除。
所有记录都是台架或隔离环境的观测，不是真实车辆或 CAN 验收。

| 文件 | 内容 | 日期 | 大小（字节） | 状态 |
| --- | --- | --- | ---: | --- |
| `orin-isx031-cpu-20261026.json` | PR25 的 Orin ISX031 CPU 台架：mock 适配器、CAN down，无 VCU 握手与驾驶输入。属于 PR25 基线 | 2026-10-26 | 4,284 | 有效（历史） |
| `native-receiver-20261027.json` | 原生接收端探针的实现提交、源与二进制哈希、环境和安全约束 | 2026-10-27 | 23,462 | 有效（历史） |
| `mac-receiver-ab-20261027.json` | Mac 原生接收端与网页接收端 A/B（`mac-receiver-ab-evidence-v1`），含测量时与最终校验的二进制哈希 | 2026-10-27 | 23,573 | 有效（历史） |
| `hybrid-ui-long-ab-20261027.json` | 网页视频与原生视频的长时 A/B（`hybrid-ui-long-ab-20261027-v2`） | 2026-10-27 | 130,113 | 有效（历史） |
| `pr26-review-20261028.json` | 审查修复记录：TURN UDP 验收的变更、验证环境、边界（各路径 WAN 秒数均为 0）、源与日志哈希 | 2026-10-28 | 8,440 | 有效（历史） |
| `dualstack-fallback-20261028.json` | 双栈 ICE 诊断与有界 TURN 回退：只含代码与隔离回环自动化测试，没有 Orin、相机/CAN 或独立公网性能测试 | 2026-10-28 | 13,953 | 有效（历史） |
| `ipv4-udp-diagnosis-20261028.json` | IPv4 UDP 诊断：同机 Chromium 套接字隔离与被取消的局域网冒烟，没有 WAN 与 30 分钟验收 | 2026-10-28 | 7,824 | 有效（历史） |
| `pr26-codec-recovery-20261028.json` | 编解码与 IDR 恢复路径验证（定向 CTest 12/12）、未验证项清单、独立 freshness 测试的 CI 依赖修复 | 2026-10-28 | 11,469 | 有效（历史） |
| `pr26-packaging-ci-20261028.json` | 打包 CI 失败与修复记录（libnice 依赖等），含 macOS 产品直连集成测试两次不同症状的失败 | 2026-10-28 | 4,081 | 有效（历史） |
| `5g-direct-20261028.json` | 蜂窝直连媒体实测（`mine-teleop-pr26-cellular-direct-v1`）：路由观测、安全约束、被排除的尝试 | 2026-10-28 | 11,730 | 有效（历史） |
| `5g-direct-recheck-20261029.json` | 蜂窝直连复测：路由、IPv6/IPv4 原生、STUN | 2026-10-29 | 2,530 | 有效（历史） |
| `windows-cellular-udp6-20261029.json` | Windows 蜂窝 IPv6 原始 UDP 可达性，只是原始 UDP，不是 ICE 或媒体 | 2026-10-29 | 1,002 | 有效（历史） |
| `windows-native-viewer-20261029.json` | Windows 原生查看器短时功能冒烟（Orin 蜂窝），无驾驶输入、无 CAN | 2026-10-29 | 5,112 | 有效（历史） |
| `windows-orin-media-safety-20261029.json` | 严格 mock 控制启动、仅媒体配置与两次媒体尝试 | 2026-10-29 | 4,614 | 有效（历史） |
| `windows-orin-webrtc-20261029.json` | Windows Edge 无头对 Orin 蜂窝双 ISX031、UDP 直连、仅媒体 | 2026-10-29 | 2,914 | 有效（历史） |
| `windows-orin-raw-udp-20261030.json` | 原始 IPv6 UDP 可达性，不是 WebRTC、媒体、控制、稳定性或延迟验收 | 2026-10-30 | 1,329 | 有效（历史） |
| `windows-orin-5g-input-20261030.md` | Windows/Orin 5G mock 输入与控制：隔离测试身份、mock 适配器、中性输入 | 2026-10-30 | 11,994 | 有效（历史） |
| `windows-orin-5g-mock-20261030.md` | Windows–Orin 5G mock 控制观测，台架观测，不是车辆或 CAN 验收 | 2026-10-30 | 12,379 | 有效（历史） |
| `windows-orin-5g-control-20261031.md` | 5G mock 控制验收：要求的不间断直连控制、无停车验收**失败** | 2026-10-31 | 5,759 | 有效（历史） |
| `windows-orin-5g-control-followup-20261031.md` | 上一份的后续记录 | 2026-10-31 | 14,272 | **部分失效**（含紧凑格式与 DC/WSS 副本的章节） |
| `windows-orin-5g-repeat-20261101.md` | 5G 重复 mock 对比，隔离的 `test_bench_pr26` 身份 | 2026-11-01 | 6,591 | 有效（历史） |
| `windows-native-nine-20261101.md` | Windows 原生九种输入 mock 测试 | 2026-11-01 | 8,797 | **已失效**（比较依赖已删除的 DC/WSS 副本） |
| `windows-orin-5g-native-compact-full-20261101.md` | 原生紧凑控制与全量遥测实验 | 2026-11-01 | 4,559 | **已失效**（实验已删除） |
| `windows-native-ui-preflight-20261101.md` | Windows 原生 UI mock 预检，也是旧 PR 说明引用的详细版本、输入与失败证据 | 2026-11-01 | 78,702 | **部分失效**（含紧凑格式与 DC/WSS 副本的章节） |
| `pr26-retry-root-cause-20261105.md` | 集成夹具三处重试的根因排查：**没有找到根因**，重试已带原因计数 | 2026-11-05 | 5,150 | 有效 |

## 关于迁出

计划里的"迁出原始 JSON"还没有执行，也不会在你同意之前执行任何迁移或删除。

- 全部 25 个文件合计约 450 KB（JSON 约 250 KB，Markdown 约 200 KB），最大的是 `hybrid-ui-long-ab-20261027.json`（127 KB）和 `windows-native-ui-preflight-20261101.md`（77 KB）。按体积没有迁出的必要。
- 按地址格式扫描，文件里没有公网 IPv4 或 IPv6 地址：IPv4 的 3 处命中是软件版本号和基准测试地址 `198.18.0.1`，IPv6 全球地址为 0。这个扫描只看地址格式，不覆盖主机名或账号。
- 如果一定要迁出，按体积优先的候选是上面最大的两份，以及 `hybrid-ui-long-ab-20261027.json` 之外的 `mac-receiver-ab-20261027.json`（23 KB）和 `native-receiver-20261027.json`（23 KB）。
  迁出后在这里留一行指向新位置，并保留文件的 SHA-256。
