# PR24 短授权运行时第一层与健康直连优先

此文件记录在 `c007391` 之后的第一层运行时改动，后续急停/反馈层另行提交。**现场实验仍默认关闭。** 用户已于 2026-10-15 07:15:53 UTC 明确同意：云暂时不可达但命令、反馈、画面正常时，当前驾驶员可在短授权剩余期内继续，到期未恢复则停车；期间不能换人接管，其他保护不放宽。用户随后要求健康直连默认优先、云转发备份，覆盖之前不偏好拓扑的最低延迟排序。

用户未批准具体租期或探测/重排门限。本文不是启用批准；不部署、不再访问待装车的旧台架，新硬件核验与现场窗口由主线程负责。

## 最新路径策略

- 控制仍为 native 唯一生产者、全局 seq、ordinary 单路发送。桥传入当前 media attempt 的实际 ICE `network_path`；只有 `p2p` 享有直连优先。P2P 仍须真实 accepted neutral 结果满足既有资格，不能凭 DC open 或媒体 RTT 驾驶。
- 健康 P2P 即使实际结果比 WSS 慢一些，也不为瞬时云更快而退出；不健康、断开、旧 owner/attempt 或实际结果停滞则按既有规则撤回输入并回退 WSS。失败后的重试仍受既有恢复等待防抖约束。
- TURN/unknown DC 不标作 P2P，保留与 WSS 的真实车端结果比较，作为云转发/未知路径的保底选择。镜像结果的实际回程和命令路径分别记录。
- 视频维持同一 PC 的 ICE all，允许正常 ICE 优先级检查 host/srflx 并用 TURN 保底；**没有浏览器业务层强制提名另一条成功 ICE pair 的能力**。当前并未证明真实 P2P 或 TURN/UDP 已成功，不能把“默认优先”写成“已实现直连”。没有新视频服务或网络配置变更。

## 参数建议与默认关闭

| 项目 | 状态 |
| --- | --- |
| cloud `experimental_direct_authority_lease_ms` | 默认 **0**；没有 YAML/CLI 开启入口 |
| cloud `experimental_direct_authority_journal` | 默认空；正租期必须显式提供稳定绝对路径、已存在目录和独占文件锁 |
| native `enable_experimental_direct_authority` | 默认 **false**；必须同时协商阶段 2 native transport |
| vehicle `runtime.experimental_direct_authority_max_ms` | 默认 **0**；不从 YAML 加载；正值不得超过已有 control-timeout 配置 |
| 600ms 租期 | **仅本机待讨论候选和 localhost fixture**，未获现场批准；不是由 command max-age 反推的云断线要求 |
| 约租期 1/3 续期，事务超时上限约 1/3 | 600ms fixture 对应约 200ms；这是独立授权 RPC 的调度/事务预算，**不是 200ms 云失联停车门禁**。实际续期时余量须实测验证 |
| 车端检查 cadence | 复用既有 watchdog 50ms 调度；不每 50ms 发心跳。实际截止执行还包含 OS 调度/锁余量，不能未经测量声称硬实时上界 |

600ms 候选是为了先评估较现有 800ms 硬断流尺度更短的独立授权限制是否有足够云事务、时钟与调度余量；**这不证明它已满足车辆允许的撤权/物理停车预算**。需同时验证续期开始 R、事务 D、时钟不确定 U、本机调度 J 满足 `R+D+U+J<L`，以及 L 加执行余量满足审查后的撤回运动预算。网络若不满足，不能扩大既有门限来让实验通过。纯边界用例也使用 80ms 等小 fixture 数值，均非产品参数。

## 已接入的真实运行时

1. **云签发、车端落地。** 新 `native-direct-authority-v1` RPC 仅接受当前认证 vehicle connection generation、活动 session 和设备凭据。grant 绑定 issuer、vehicle/driver/session、authority epoch、lease seq、本次车端随机 nonce、签发时间和绝对截止。native 的 DC 包只能引用已由车端接受的记录，不能携带新 grant 来续车端授权。
2. **车端保守单调截止。** 同时取 request-start + duration 与可信云时间上界换算的截止较早者；扣除时钟不确定度。迟到/重复 nonce、旧 lease seq、旧 issuer/身份不延长 deadline。旧授权过期后车端仅接受新的 authority epoch，先制动/失效输入，再走既有恢复门禁。普通 path epoch 与嵌套授权 authority epoch 不同，不能互相续期。
3. **车端独立执行。** 既有独立 watchdog 观察授权到期并进入 braking wait；之后新鲜 DC 包仍被拒，不调用共同服务，不能续控制 watchdog。原 degraded/hard-timeout、profile 清除、视频、输入、CAN 和时钟保护保持原值。收到认证拒绝即撤销；网络超时/408/429/5xx 仅保留原 deadline，不续权。
4. **原生端受限继续。** 只有已确认 P2P/合格 DC 路径、有效车端已接受授权、当前 owner/attempt、输入/视频/时钟和既有实际结果恢复条件都健康，才不因云 socket 失败而撤销当前直接流量。云入口 ACK 不再替代直接路径的真实车辆结果。授权到期/更换代次让旧输入失效；单纯同 epoch 的正常续租不会每次要求重新按键。
5. **分开云故障和真实拒绝。** 保留健康当前 PC 的代码仅用于暂时网络/服务错误；认证/会话拒绝或协议错误仍停止。真实时钟不可信仍按原保护，不能靠“云断线”解释成时钟继续可信。
6. **管理面不离线提升。** 新模式下参数更新和 VCU connect 要通过现有认证云交付；允许有效短授权在 ordinary path 尚未完成协商时安全完成管理 bootstrap。DC 上 VCU disconnect 保留，不能因云故障阻断停车请求。原首次参数/停车条件和握手门禁不变。

## 防双重授权与持久记录

- 云记录每车已签发授权的单调保留截止。session end/revoke 不立即清除此保留；保留期内另一驾驶员和替代车端 connection 的注册均被拒。不会因为云 UI 已显示注销就同时发第二份仍可使用的控制权。
- 正租期必须有稳定 journal 和独占写者锁。grant 返回之前，先写临时文件、刷盘、原子替换并提交目录（Windows 使用对应 write-through 替换）。重试同 nonce 也重新确保持久性，不因前次写失败而返回未持久化 grant。
- journal 只存非秘密 grant 身份和期限，不存 MAC key、设备 token 或密码。日志写串行，但刷盘时释放普通 SignalingService 控制邮箱互斥锁；刷盘失败保守保留内存 reservation，返回不可用，不发新授权。
- 重启读取先前 grant 的 epoch，并在新进程单调时钟上保留**完整旧租期**，不靠旧 UTC 截止判断已过期。新 issuer 与递增 authority epoch 拒旧状态。即使关闭新授权功能，也必须保留同一 journal 配置以尊重未过的保留期。
- 此实现是稳定本地 journal 的**单写者云服务**；不能直接作为多机 HA 的强一致租约实现。删除/替换 journal、换存储路径或同时跑独立账本会破坏这个前提，启用评审必须核对。新的驾驶仍经过既有车端 profile/VCU/CAN 等门禁；reservation 到期不是物理已停车证明。

## 离线证据与尚未完成

- 新 `mine-teleop-direct-authority-tests` 运行真实云 handler、真实文件锁/持久记录及共同 `LogicalTimeControlService`：nonce/时钟/迟到/重放、DC 不续权、到期独立制动与原 watchdog、撤权后抢占拒绝、重启保留和同 nonce 刷盘失败恢复。
- `control_transport_tests` 新用例要求即使 WSS 实际基线更快，合格 P2P 仍优先；并验证真实失效和完成 fallback 屏障后的防抖重试。unknown/relay 原比较用例保留。
- localhost integration 运行真实 native/cloud HTTP+WSS、签名命令/结果、共同服务及模拟 DC：关闭真实云 socket 后短授权内继续推进；恢复云不赶走健康 DC；再次失联后独立车端线程到期制动，不依赖 native 重连/页面调度。
- 本机 native/cloud 编译与上述测试不能替代完整 Linux/GStreamer 车端编译。Mac 缺该依赖，先前既有 Docker daemon 有 containerd 临时挂载 I/O 故障；未重置或删除数据，交 PR24 Linux CI。
- **本提交没有接入 stop-only 急停双路或独立反馈 epoch。** 当前急停仍 WSS；阶段 2 结果回报/状态保底仍在。相应模型见 `cpp/tests/path_selection_models`，后续会分别做真实接入与测试，不能由模型通过宣称完成。
- 需要后续完整车端/新硬件窗口验证 authority 过期后的完整 UI 松开重按、CAN/VCU 恢复、真实 P2P/TURN、异步回调与线程停止、最坏调度余量及刷盘故障注入。本层已触发原有恢复边界，但未声称完成物理验收。

旧台架探测的既有前台 SSH 已结束、退出码 0。本机对应 probe wrapper 检查为空；此轮没有重新连接旧台架，不部署、不启动现场会话、不修改网络。
