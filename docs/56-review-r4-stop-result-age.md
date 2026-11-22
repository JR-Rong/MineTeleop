# 审查 R4：当前停车确认计算完整年龄

`review-r4-before.log` 保留真实 localhost native/cloud、签名 STOP、
真实 receiver/common service 的失败反例：车端接受停车后完成本地复位，
旧成功结果延迟260ms才送回，原生端仍错误地给出当前锁存确认。

修复沿用原200ms新鲜度，不增加新阈值：当前确认的完整年龄是
“原生首次发布→结果返回的RTT + 返回后至当前的时间”。只看最近收到
结果的时间不再足够。计算避免加法溢出，身份、authority/reset epoch、
accepted/latch 和软件/物理区分等原有条件保持。

迟到的真实成功结果仍保留为历史证据，API返回
`historical_software_latch_result=true` 和原RTT；
`software_latched_confirmed=false` 不意味着投递失败，也不代表当前
车端或执行机构仍锁存。物理确认仍需新鲜 CAN。

新增专门 stale-stop localhost目标验证本地复位后的迟到结果不会误确认，
并保留历史成功；原集成目标继续覆盖正常快速确认、WSS先阻塞后停车、
短授权和独立反馈。日志 `review-r4-after.log`。没有改 R5 的一次尝试
消费行为，没有现场访问或部署，实验保持默认关闭。
