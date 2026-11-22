# 审查 R7：明确初始状态，恢复丢失的首次选择

`review-r7-before.log` 保留失败：native 首次1/from0选择没有到车端，
新attempt使候选变成2/from1；车端仍epoch0，6次精确签名结果被忽略，
无法初始化反馈。

## 有界对账

未初始化的车端现在只在请求的authority/attempt与自身当前范围相符时，
产生明确 `state=uninitialized, epoch=0, from_epoch=0, path=wss` 状态，
附当前authority、attempt及status floor。完整结果连同精确请求由车端
MAC签名。null、缺字段和无标记零值不代表重置许可。

native 只在首次反馈尚未确认时、精确匹配本机实际发送事件且当前
authority/attempt相同的已认证零状态上，重新发1/from0选择。
已有确认状态不能被零值降级。新会话按原reset路径重新初始化，
不靠旧包修改会话/授权。旧epoch、旧attempt、旧authority或未绑定
结果不能推动该恢复。

如果同一精确结果还是原命令传输轴的 `transport_epoch_conflict`
拒绝，会交给**既有命令CAS对账**，防止两条轴互相等待。该例外必须
accepted=false、transport_admitted=false，并命中本机命令戳；
它不计反馈健康、不授权运动，也不制造重复普通命令。

浏览器只为当前尚在选择中的明确零状态转交原字节结果，仍由native
验证MAC与实际发送记录；浏览器不能自行设置或接受epoch0。

## 本机证据

- 生产反馈类：丢首次选择后新attempt从零恢复；null/缺标记/旧范围/
  已确认状态拒绝降级；命令和反馈同时冲突时恢复现有两条CAS。
  正常P2P优先、备用、候选状态及有界历史仍回归。
- courier **13项通过**，含零状态的当前pending范围门禁。
- 真实localhost native/cloud故障注入：首次反馈选择在车端接收前丢失，
  经真实信令交付新offer后切attempt，使用签名零状态完成初始化；
  独立目标 **1/1通过，1.56秒**。
- fixture 的车端长期截止改用session discovery/认证短授权响应的真实
  缓存，移除了每次准入传 `utc()+10000` 的测试捷径。

日志 `review-r7-after.log`、`review-r7-courier.log`、
`review-r7-zero-loopback.log`。首次扩展集成的fixture在新offer尚未
交付时先切了本地scope，被已有scope门禁正确拒绝；已保留该日志并
将fixture改为确认真实offer交付后才使用新scope，没有放宽生产门禁。

实验保持默认关闭，期限无变化，无台架、设备、部署或推送。
完整Linux/GStreamer和跨机测试仍交主线程协调；这些是协议/软件
验证，不是硬件20Hz、物理停车或真实P2P证明。R5语义仍未改变。
