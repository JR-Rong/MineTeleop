# 审查 R3：短授权续期同时校正认证长期截止

修复前 `review-r3-before.log` 保留失败：实际授权 worker 收到正常
短授权响应后，短授权有效，但长期缓存仍为1049；云时间1050时真实
Fence以 `transport_authority_unavailable` 拒绝新鲜DC包。

修复不删除双重授权门禁，也不调大期限。云在当前认证短授权响应中附带
该活动 session 的 `control_token_expires_at_utc_ms`，并与同份公开
grant一起持久化；同 nonce 重试仍返回原值，不延长短授权。

车端仅在认证RPC响应满足以下条件后更新既有长期缓存：截止是有界整数，
不早于该短grant截止，且 nonce、issuer、session、vehicle/driver、
generation、epoch和时钟等原有grant校验实际接受。错误nonce和未接受
响应不更新缓存；DC自报截止没有该入口。

本机测试使用实际 worker方法、真实 Lease/Fence，信令为无网络替身；
更新后旧长期缓存不再误停，错误nonce仍不能改长期截止。真实云handler
测试核对返回的当前长期截止，并拒绝短grant越过长期截止的畸形响应。
worker、cutoff、既有cloud/journal/authority目标 **3/3通过**，日志
`review-r3-after.log`。

实验仍默认关闭；600ms/80ms只为fixture。此变化要求启用实验时三端
匹配，旧实验云缺少该截止字段会失败关闭。完整Linux/GStreamer和
跨机验证待主线程协调，没有台架访问、部署或推送。
