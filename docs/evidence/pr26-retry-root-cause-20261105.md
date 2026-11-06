# PR #26 集成夹具重试：根因排查记录

日期：2026-11-05。范围：`cpp/tests/control_transport_integration_tests.cpp` 里三处"夹具重试"。

## 结论（先说结果）

**没有找到根因，也没有在本地复现过任何一次重试。** 三处重试保留，但现在每一次都会带原因打印并计数，
CI 日志里会直接出现证据。不要把这份记录读成"问题已解决"或"重试没有必要"：它只能说明在下面列出的本地条件下重试没有被触发。

## 三处重试

| 位置 | 触发条件 | 重试方式 |
| --- | --- | --- |
| `product_startup_and_p2p_cloud_recovery` | 车端 `safety_state == TimeoutBrake` 且最后结果为 `session_control_profile_required`，即车端 800 ms 内没有收到有效命令，真的走到了硬看门狗 | 丢弃整个夹具，用全新夹具重跑全部断言，最多 3 次 |
| `product_recovery_withdraws_unfetched_motion` 内层 | 驾驶端尚未发布非零动作，因为动作采样赶上了路由边界的重新整定 | 在 ACK 看门狗到期之前重新松键再按，最多 3 次 |
| `product_recovery_withdraws_unfetched_motion` 外层 | 待撤回的发布在检查时已超过 `kNativeBridgePublicationMaxAgeMs` | 整轮重来，最多 3 次 |

代码注释把原因归于"托管 runner 可能让发送线程被延迟一个周期以上"。这是当时在 CI 上看到的现象，本地没有留下可对照的运行记录。

## 本次做了什么

1. 给三处重试加了标记：`[RETRY] scenario=… attempt=… reason=… wall_minus_steady_ms_since_start=…`，
   每个正常返回路径前打印 `[RETRY-SUMMARY] total=N`。`wall_minus_steady_ms_since_start` 是 `utc() - steady()` 相对本进程第一次重试时的变化，
   用来判断重试是否与挂钟被调整同时发生。
2. 本地实验（Colima 虚拟机里的 Ubuntu 22.04 容器，aarch64，8 核，`--product-only` 即三处重试所在的场景）：

| 条件 | 运行次数 | 失败 | 出现重试 |
| --- | --- | --- | --- |
| 默认 | 30 | 0 | 0 |
| 8 个忙循环与测试抢 8 个核 | 20 | 0 | 0 |
| 容器限制在 2 个 CPU，6 个忙循环 | 20 | 0 | 0 |

3. 确定性实验（挂钟回拨，服务层，不依赖虚拟机的偶发行为）：让车端服务的逻辑时间倒退一段后继续按 20 Hz 送命令。

| 回拨量 | 被拒收的命令 | 之后的状态 |
| --- | --- | --- |
| 0 / 50 ms | 0 | `CONTROL_ACTIVE` |
| 100 ms | 1（`receive_time_reversed`） | `CONTROL_ACTIVE` |
| 150 ms | 2 | `CONTROL_ACTIVE` |
| 250 ms | 4 | `CONTROL_ACTIVE` |

## 已排除或不支持的假说

- **挂钟被调整导致车端走到 800 ms 硬看门狗：被证伪。** 这个环境里挂钟确实会倒退：Lima 的 host agent 会校正客户机时钟，
  日志里有 `Time sync: guest clock adjusted (was 103ms off)`，相邻两次校正相隔 30 秒；实测 45–60 秒内挂钟倒退 1–5 次、每次约 100 ms。
  在 `colima.yaml` 和 `lima.yaml` 里没有找到相关配置项（没有查阅 Lima 的文档或源码，不能断言确实没有）。
  但上表说明一次这样的回拨只会丢掉 1–2 个 20 Hz 命令，车端不会离开 `CONTROL_ACTIVE`，更到不了硬看门狗。
  （这个挂钟问题确实造成了别的偶发失败，见下一节，但不是这三处重试的原因。）
- **CPU 抢占让发送线程被延迟：本地不复现。** 8 核加 8 个忙循环、2 核加 6 个忙循环各 20 次，都没有重试。
  这不排除更小的 runner、更差的调度或不同的操作系统（Windows 托管 runner）。

## 与此相关、已经确认的环境偶发（不属于这三处重试）

- `mine-teleop-media-time-sync-retry-loopback`（`cli-over-budget`）：阶段 2 基线二进制 4/12 失败，阶段 3 之后 4/12 失败，
  空闲与满载下失败率相同。该检查比较客户端用挂钟和单调钟读到的同一次交换耗时（`cpp/src/http.cpp` 的四时间戳校验），
  与挂钟被调整的特征一致。**未对这一点做干预验证**（没有改动测试环境的时钟）。
- 阶段 3 的瞬态保持在夹具里偶发 `clock_reversed` 升级：已通过让这些夹具的车端服务使用真实 `steady_clock` 消除，42 次连续运行零失败。
  这是对"挂钟倒退会造成偶发失败"的**干预验证**。

## 如果以后要继续查

1. 推送后看 CI 日志里的 `[RETRY]` 和 `[RETRY-SUMMARY]`。有 `wall_minus_steady_ms_since_start` 的跳变，说明与时钟调整有关；
   没有跳变而重试仍发生，就是别的原因，这时 `reason=` 字段说明是哪一处。
2. 如果在 CI 上也长期没有重试，说明这些重试已经没有必要，可以删除；是否删除应由看到数据的人决定。
3. 如果重试确实发生且与时钟无关，需要对照车端 `control_hard_stop_transition` 诊断、驾驶端 trace 里的 sender 唤醒延迟和 socket 锁持有时间继续定位。
   判断是夹具调度问题还是生产代码竞态，之前没有依据。
4. 其他夹具仍然把 `utc()` 当作车端服务的单调时钟。如果它们出现偶发，先用 `steady_clock` 做干预验证。
