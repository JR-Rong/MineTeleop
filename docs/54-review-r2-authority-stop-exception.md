# 审查 R2：授权停车异常不能逃出 worker

基于 `fb26704`，按冻结审查 R2 先保留反例，再修复。实验仍默认关闭，
安全数值没有变化；600ms 仅测试 fixture。只在本机操作，不影响测试
agent 的固定版本或台架会话。

## 反例与最小变更

新的测试从当前 `webrtc_media.cpp` 原样提取完整授权 worker 和 tick，
真实链接共同控制服务，只将 Mock adapter 的 safe-stop 注入抛错。
认证 RPC 返回401时，原 catch 内停车异常逃出 jthread，测试进程的
terminate handler 以86退出；不生成 core，不接设备或网络。
`review-r2-before.log` 和 `review-r2-before-receipt.json` 保存失败及散列。

- 正常情况下仍立即调用共同服务请求制动，没有新增等待或超时。
- 新的不可抛出停车边界捕获物理停车失败，先保证短授权 revoked，并
  标记停车未确认。不会把 failed safe-stop 当作成功，也不让异常从
  worker 的 catch 再逃出。
- 下一次既有 tick 将该故障送入原有诊断、safe-stop/close 和禁控路径。
  close 再失败时仍报告需要物理急停，不宣称物理执行成功。
- 授权 worker 整体增加最终异常边界，覆盖初始配置、随机数等不在内部
  RPC try 中的异常，统一撤权并请求上述故障关闭。
- 授权到期的制动调用移入 tick 既有 try，避免 watchdog 外层静默吞错。
- `fb26704` 新增的 WSS 拒绝分支复用同一停车边界，避免 catch 内再次
  出现同类物理异常逃逸。身份/epoch 撤权语义没有改变。

## 本机验证

`mine-teleop-authority-worker-tests`：3组真实源码方法测试通过，覆盖
RPC拒绝+停车失败、授权到期+停车失败、worker入口异常；共同服务
被关闭且控制失效，故障诊断不被跳过。

与实际 WSS 入口撤权测试一起 CTest **2/2通过，1.29秒**，日志
`review-r2-after.log`。这些测试使用真实 lease/common service，信令和
Gst 是测试替身，不是完整 Linux/GStreamer ABI 或物理停车验证。

R3/R4/R6/R7继续独立处理；R5未入队 STOP 的消费语义尚未修改。
