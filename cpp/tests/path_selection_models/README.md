# PR24 后续选路：隔离纯模型与模拟测试

本目录仅为设计审查准备可执行的状态机/故障用例，**没有接入产品，也没有新建授权时限、网络门禁或产品配置**。用户是否允许云短断期间健康 DC 在短授权内继续仍待答复；这里不会默认选 A/B，也不会把 `caller_authorized` 自行变成 true。

## 可独立集成的最小分层计划

| 层 | 新增文件 | 本次准备 | 后续实际接入前还需证明 |
| --- | --- | --- | --- |
| 1 急停 | `stop_model.hpp`、`stop_model_tests.cpp` | 有界两路分派计划；stop-only 前提；reserve/apply/commit 去重；同 seq 冲突；已处理缓存/淘汰重放；旧权威/复位代次隔离；离散事件黑洞模拟 | 真实 stop-only 版本协议、全局 native seq、实际 MAC/源时间校验；先 latch 后 ACK；独立分派不被同一阻塞锁串行化；原共同服务及 CAN 结果 |
| 2 独立反馈 | `feedback_model.hpp`、`feedback_model_tests.cpp` | 独立回程 epoch CAS；四类最新状态先验 scope 再排序；成对 return probe 共同去程抵消、nonce/cohort/事件绑定、单个 in-flight、过期/倒退/重复拒绝 | DC/WSS 真实四种去回程组合；签名字段与消息身份；原 reducer/四类快照；实际 backpressure；反馈 gap 与原恢复门禁；限额 probe 的调度预算 |
| 3 持续选择 | `switch_model.hpp`、`switch_model_tests.cpp` | 复用既有 advisory policy；健康路径性能试验等自然 neutral；真实结果资格；一次一轴；回程更换使去程 cohort 失效；硬故障不等驻留；安全中断退休 ticket | 真实序号/intent 精确关联及接受结果；原 epoch sender/receiver 接入；既有 release/repress、视频/时钟/CAN/readiness；p95/median 样本与开销；实际 ICE 可选择能力 |

所有文件均新增在这个测试目录；没有修改 `d54585a`/`6db3cbb` 的 header、发送器、接收器、主 CMake、CI 或运行时。层 3 只 include 第一阶段 `lowest_latency_path_policy.hpp`；独立测试不要求阶段 2 或设计文档提交已经集成。可以在已包含 `d54585a` 的分支直接 cherry-pick 本次测试提交，无需将其他直连实验一并启用。

建议主实施任务先把本目录作为独立 CI 命令运行；后续每层运行时适配器分别提交，将同一契约用例接到实际实现上。不要因为模型通过而给现有实验加启用入口。反馈/急停/选择的真实适配器会触碰主任务正在集成的代码，此阶段没有写这些文件。

## 无网络复现

```sh
cmake -S cpp/tests/path_selection_models -B /tmp/mine-teleop-path-models
cmake --build /tmp/mine-teleop-path-models -j 3
ctest --test-dir /tmp/mine-teleop-path-models --output-on-failure
```

仅需 CMake 和 C++20 编译器，不下载依赖、不连接云/台架、不启动 UI、不依赖 GStreamer/OpenSSL。可单独运行 `path-stop-model-tests`、`path-feedback-model-tests` 或 `path-switch-model-tests`。警告作为错误。三个 CTest 共包含 16 个具名用例组（急停 6、反馈 5、选择 5），其中急停缓存用例推进 10,000 个身份以验证固定容量和淘汰后不重执行。

## 证据和假设边界

- `Preconditions.verified/caller_authorized` 是**外部已经完成验证的前提占位**；模型不是密码库，也不能作为生产验签/授权判定使用。没有 authority deadline、续租循环、云可达性或 five-minute token 容忍。前提不成立时拒绝事件，不定义用户尚未确认的云失联行为。
- `Digest` 是公开 fixture 的事件身份，不计算散列、不签名。scope/epoch/seq/digest 用例证明模型的状态隔离，不能证明真实消息已认证。与现有 v1 MAC/序号/时间字段兼容需要之后的适配器/协议评审。
- 急停分派只是最多两个 route 的计划。离散事件模拟把两次分派放在同一逻辑时刻，验证“若独立分派则慢路不挡快路”；它不能证明真实线程或 socket 不会阻塞。`Full` 仅表示去重预留容量不足，不是忽略急停或取消本地运动撤回的许可。实际车端共同接收锁应防止同时产生无界 pending apply。
- 急停重复返回软件结果，不再次 apply；没有物理停车、CAN、安全复位或 watchdog 的模拟实现。实际适配器必须保证任何重复 ACK/probe 都不续原 watchdog/授权，且旧权威/本地复位代次不能影响新会话。
- 成对反馈只给同一个车端事件通过两条回程的 round-trip 差值。共同去程固定才能抵消；新 forward/feedback/media cohort 必须重建。没有可信车端时钟就不输出绝对一向延迟。probe 既不执行控制，也不授予驾驶资格。
- `SwitchModel` 只记录“建议→等待 neutral→屏障→真实结果资格→确认”的测试生命周期。`matched_native_send` 必须由实际发送记录证明；probe/云入口/ICE 值无法资格确认。模型确认不会运行真实 input recovery/release-repress；实际适配器仍须保留这些门禁。安全中断后保持 `SafetyHold`，没有自动恢复接口。
- 选择模型复用的 200ms/3 样本/5ms/10%/100ms/300ms 均是原 fixture 的**显式测试数值**。急停容量 4、状态批次 16、probe deadline 和结果计数也是模型测试边界，不是新产品默认或安全门限。持续排序的生产 p95/median 估计器与带宽/CPU预算尚未实现。
- 同一个 PC 的 P2P、TURN/UDP、TURN/TCP 不是三个同时可任意选择的业务通道。测试 `NetworkPath` 表示调用方已证明实际可选择的候选；`selectable=false` 的 ICE pair 不能被选中。没有媒体 ICE restart/强制提名或最低呈现延迟实现。

## 本次可执行验收

| 用例组 | 模型断言 |
| --- | --- |
| 急停主路快慢反转、主路黑洞 | 择优不硬编码 DC；备用有界分派；模拟首个有效到达锁存一次 |
| 急停双路重复/冲突/缓存满/淘汰 | pending/cached 区分；同 seq 异内容拒绝；不淘汰 pending；淘汰身份只拒重放，不重 apply |
| 急停旧身份/复位/未验证/未授权/过期/非 stop-only | 全部拒绝，不由“路径健康”补足前提 |
| 反馈 CAS/旧 epoch 高 seq/旧 attempt | 旧流在更新水位之前拒绝；重复选择幂等；不能跳 epoch 或倒退 floor |
| 反馈四类乱序/重复/伪高 seq | 先验证再按类保留最新，排序后提交；保留全部四类有效新状态 |
| 回程共同前缀抖动、排序反转、重复/过期/错 nonce/事件/时钟倒退 | 只完整配对产生一个样本；差值不受共同去程抖动影响，不能制造绝对一向指标 |
| 健康路径逐渐落后、微小差异、静态样本、新 attempt | advisory 稳定/驻留/新进展规则成立；自然 neutral 前不启动性能屏障 |
| probe 很快但无真实结果、结果迟到/重复/错 scope | 不确认 candidate；坏样本重置连续资格；不能绕过原始发送关联 |
| 反馈切换/去程切换同时建议、硬失败、安全中断 | 一次一轴；新回程使旧去程 cohort 失效；硬失败立即撤回建议，旧 ticket 不能恢复 |

本次没有运行既有 native/cloud 或真实 WebRTC，因为没有修改其代码；这些集成和全 Linux 车端编译仍由主实施任务按其窗口执行。模型通过不会改变阶段 2 的默认关闭状态。
