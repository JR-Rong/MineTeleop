# session-000075：频繁控制迟到的进一步诊断

本文描述 8d13c23 的日志与修复。后续将 ACK 500 ms 主动重连改为先制动、保留
连接并有界等待恢复，新增独立阶段观测和后端能力校验，见
[ACK 制动等待与 session-000076](41-ack-braking-recovery-and-session76.md)。

## 当前结论与证据边界

**存在真实的命令积压，主要位于驾驶端 send 返回后、云端 ingress 之前。不能
唯一判断为网络中断或单连接的软件阻塞。** 车端的 2 s 门限指无合法新控制，
不是连续 2 s 完全没有收到任何报文。途中收到的迟到命令不能执行或续 watchdog。
时间均为 UTC 2026-10-13，附件版本与截断范围见 [docs/39](39-session75-stop-analysis.md)。

子 agent 按 session/seq 合并三端 trace，再由主 agent 对照当前收发源码。硬停车前
4569 次成功发送中，发送调用 P99=2 ms、最大 6 ms；发送间隔 P50=63 ms、P99=67 ms、
最大 157 ms；唤醒 lag 最大 42 ms。没有驾驶端发送线程 2 s 卡住的证据。
云端 ingress 内处理最大 1 ms、mailbox→send 最大 50 ms、send 调用最大 2 ms；
车端 control mutex 最大 1 ms、apply 最大 2 ms。但 cloud ingress 起始时间在全局
mutex 获取后，不能用这组数排除取锁前等待。

| seq | 驾驶端 send 返回 | 云端 ingress | ingress 前等待 | 车端结果 |
|---|---|---|---:|---|
| 4538 | 06:37:04.397 | 04.427 | 30 ms | 接受 |
| 4539 | 04.459 | 04.907 | 448 ms | 被最新命令替换 |
| 4543 | 04.708 | 04.922 | 214 ms | 230 ms，过期拒绝 |
| 4546 | 04.897 | 05.057 | 160 ms | 05.082 最后接受 |
| 4558 | 05.647 | 05.887 | 240 ms | 321 ms，过期拒绝 |
| 4562 | 05.896 | 06.786 | 890 ms | 914 ms，过期拒绝 |
| 4564 | 06.021 | 07.117 | 1096 ms | profile 已撤销 |
| 4571 | 06.458 | 07.497 | 1039 ms | profile 已撤销 |
| 4579 | 08.158 | 08.188 | 30 ms | 延迟恢复，但此前已硬停 |

seq4539–4543 在驾驶端单调时钟跨度 249 ms 发送、云端 16 ms 内补达；4559–4562
为 188 ms 发送、10 ms 补达；4567–4571 为 251 ms 发送、15 ms 补达。最终窗口没有
UTC/单调时钟跳变，不能解释成校时显示伪影。硬停前 ingress 前 >200 ms 共 53 条：
单次 seq926、1933–1976 中 21 条、4539–4571 中 31 条，最大 1276 ms。
前一次迟到在新 neutral seq1988 后恢复，本次持续超过现场 2000 ms 门限。

云端原日志 728/730/732/734 的 trace batch 在 04.924/05.926/06.927/07.927 连续输出，
batch_seq17329–17332 连续且 dropped_total 固定 31；其他 HTTP media_status 在
04.053/05.114/06.154/07.139 继续处理。没有整个云进程停止 2 s 的证据，不能排除
控制连接专属接收循环/ACK 写/取锁停顿，也没有 TCP 重传证据可直接归因无线掉线。

06:37:06.520，ACK 累计 seq4558 从 05.959 起停滞 561 ms，引起真实重连；首次
重连约 1 s 超时，成功发送间隔增至 1641 ms。它放大断流，发生在积压和过期之后。
本轮不把真正 ACK 停滞说成旧的“ACK 持续推进误重连”，也不放宽任何停车门限。

## 单独确认并修复的 ACK 读取 bug

驾驶端每周期 `receive_json(0)` 排空 ACK。旧客户端只在不足首两字节时保留缓冲，
若已读到头部，但扩展长度或 payload 暂未到齐，就抛 `incomplete websocket frame`，
误将正常 TCP/TLS 分批交付变成协议故障并触发重连。新增 loopback 回归先发半帧、
保持连接，再补剩余字节，旧实现可重复失败。

修复所有缺字节路径：短轮询返回 Timeout 并保留完整已读前缀，下次继续拼帧；
EOF 仍返回 Closed。已完成的合法 JSON 才返回 Message；错误头、超限长度等协议
校验仍保留。半帧和诊断采样不会更新 ACK 进度、命令 freshness 或 watchdog。
session-000075 的失败只有 ACK 真停滞、连接超时和握手超时，没有此异常，**不能
把该已确认缺陷宣称为这次停车根因**。

## 本轮日志与下一次现场判定

复用已有有界异步 trace，补云端完整帧/解析/入队前 mutex/权限检查/循环间隔；
driver/cloud 每秒采集真实 socket 的 TCP RTT、重传累计、发送/接收缓冲，驾驶端
失败前额外采集。字段语义及代理 hop 边界见 [docs/24](24-vehicle-runtime-diagnostics.md)。
旧 session-000075 没有当前已有的 ingress ACK send 日志，应核对现场云端实际包
source_git_sha；仅确认驾驶端页面新版本不足以证明三端日志齐全。

下一次使用 PR24 同一提交的三端包，保持普通 control trace 开启，导出三端完整
会话日志并保留 manifest。按同一 seq 和 socket connection 比较：

- sender sample/wakeup 增大：驾驶线程或本地锁/调度；云端 mutex/check/gap 增大：
  服务 handler 等待；云端 ACK send 耗时增大：ACK 写路径阻塞。
- sender 正常且该 TCP hop 的重传/RTO、待发积压增长：传输路径压力；仍需与实际
  peer、TLS 代理和其他进程活动对齐，不能仅凭一次 TCP 状态认定无线硬断网。
- cloud 完整帧已收到后才等待很久：软件入队前阻塞；recv assembly 很久：分批
  交付或 recv 线程调度，结合接收未读缓冲和 TCP 统计继续区分。

默认硬门限 800 ms、现场 profile 2000 ms、过期 200 ms、ACK 真停滞 500 ms、
128 未确认包上限及现有重连决定不变。修复只影响半帧保留和可观测性，没有实车
操作、现场网络测试或部署验收。

## 验证

Docker control 回归通过 45/45：覆盖短/16 位/64 位头和 payload 分批交付、零时限
轮询、保留前缀期间继续发新控制、后续帧不被吞掉、半帧 EOF、实际 loopback TCP
计数和采样限频；云端单元证明 100 ms 拼帧延迟单独记录，不支持 TCP 的 socket
不会伪造零计数；三跳测试验证新增时序字段与敏感信息不泄漏。原有真实 ACK 停滞
及回连测试保留。Docker 完整 CTest 24/24 通过（68.39 s）；跨平台打包结果在
对应提交的 PR CI 核验。

API 依据：[libcurl raw recv](https://curl.se/libcurl/c/curl_easy_recv.html)、
[Windows SIO_TCP_INFO](https://learn.microsoft.com/en-us/windows/win32/winsock/sio-tcp-info)、
[Linux TCP_INFO](https://man7.org/linux/man-pages/man7/tcp.7.html)；macOS 字段单位依据
本机 SDK `netinet/tcp.h`，Windows v0 ABI 对照 Microsoft win32metadata 的 mstcpip.h。
