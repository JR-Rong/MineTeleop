# PR24 明确云撤权不能作为短租约网络宽限

本修复基于冻结检查点 `0b2f18d`。仍默认关闭所有直连实验；600ms
仅为本机 fixture，不是生产租期或启用批准。未访问任何台架、部署或推送。

## 先保留反例

真实调用链：云 WSS 循环的授权检查失败后发送
`{event: signaling_authority_lost, error: ...}`；车端
`handle_native_control_websocket_envelope` 先检查 `error` 并抛普通
`runtime_error`；外层只调用 `note_native_control_transport_error`。
该路径更新连接故障/旧包 cutoff，却没有撤销 `DirectAuthorityLease`。
DC 的准入仍检查这份有效短授权，所以新鲜普通 DC 包可继续在剩余期内
通过。这是错误的明确拒绝分类，不能以 Mode A 的暂时不可达宽限解释。

修改运行时代码前，将当前源码 WSS 入口提取到一个本机测试，运行真实
短授权类和共同 `LogicalTimeControlService`。在旧期限尚未到时送入
可信明确撤权，测试失败：

`COUNTEREXAMPLE: trusted explicit WSS denial left accepted short grant live`

`authority-denial-before.log`、`authority-denial-before-entry.inc` 和
`authority-denial-before-receipt.json` 保存失败、提取源码及散列；对应
HEAD 是 `0b2f18d3f62d69a6735c13adea15a94406629ca8`。

## 修复后的边界

- 车端在普通 `error` 分类前处理明确撤权。在原公共控制锁内立即将
  当前短授权置为 terminal revoked、失效已信任挡位，并要求共同服务
  进入原有 control braking wait。之后的新鲜普通 DC 包在共同服务
  调用前被拒，不能刷新控制 watchdog。
- 原生端的控制 WSS 同样先处理明确撤权，关闭本地对应授权并失效输入。
  从车端返回的签名 revoked 状态仍会关闭原生端授权。不能靠重新按键、
  重连 socket 或撤权前已在途的续租响应复活该授权。
- WSS 握手原先把 HTTP 401/403/404/409 等状态丢成通用 runtime error。
  现在保留为既有 `HttpStatusError`，认证/会话拒绝按连接绑定范围撤权；
  408/429/5xx 仍沿用原有暂时服务故障分类，不获得新授权或新期限。
  可信云发送的其他显式 `error` 拒绝也不能伪装成没有收到报文的网络故障。
- 没收到可信拒绝、只是断开/超时/暂时服务不可用时，不撤销剩余授权，
  也不延长它。独立车端期限和原有安全门禁照常执行。这保留用户批准的
  Mode A 行为。

## 旧通知不能关闭新会话

每个认证控制 WSS 在建立时捕获公开授权身份：协议、issuer、vehicle、
driver、session 和 vehicle connection generation。接收者先将该
连接身份与当前配置核对。这个调用入口不接收 DC 或浏览器声明的撤权。

云为新通知增加 `authority_scope`，使用该连接的身份及对应 journal
中最新签发的 authority epoch。正 epoch 只作用于接收者当前相同 epoch；
可识别的旧/其他身份或不匹配 epoch 被忽略，不产生新的恢复边界。
epoch0 表示该会话尚无具体 grant 的会话级拒绝，不能用于重新授权。
可信当前连接上的畸形撤权范围按拒绝处理；可明确识别的旧身份优先忽略。

兼容旧云的不带 scope 通知：它表示**该已认证连接绑定会话整体被拒绝**。
即使连接跨过正常续租，它也会关闭该当前会话；旧连接绑定的 session、
issuer 或 generation 与新配置不符时则不能影响新会话。这是有意保守
的 legacy 行为，不将无范围拒绝解释成“可能只撤销某份旧租约”。

重复撤权只生效一次，不重复推进 rearm revision 或再次执行制动请求。
现有 `revoke()` 是当前配置的 terminal 状态；需要新授权会话配置才能
重新参与授权流程，不能从 DC 离线更新。

## 验证与范围

- 修改前反例失败已保留；修改后实际车端 WSS **入口源码片段**测试
  4组通过：明确拒绝立即撤销并进入共同服务制动等待、纯传输保留原截止、
  重复/在途续租不能复活、旧身份/旧 epoch/legacy scope/畸形类型。
  这是实际入口片段与真实 lease/common service，不是完整 GStreamer
  接收循环或 ABI 验证。
- 最终离线 CTest **8/8通过**，回归覆盖该入口、反馈实际发布方法、反馈生产类、停车、短授权、
  普通传输、JS和ICE页面。
- 真实 localhost native/cloud HTTP+WSS 集成由云撤销活动 session，
  在旧短租约仍有效时收到带 scope 的拒绝：车端授权立即撤销、共同服务
  进入制动等待，重新签名且时间新鲜的普通 DC 包仍被拒；实际原生端也
  标为 revoked。另用本机 HTTP 服务验证 WSS upgrade 的
  401/403/404/409 与503不会丢失类型区别。
- 原有云短断流继续/恢复、独立到期停车、停车阻塞反例、独立反馈切换、
  native ACK 与 ICE 集成最终 **3/3通过，12.70秒**，
  2026-10-15 09:43:21–09:43:33 UTC。日志是任务根目录的
  `authority-denial-*-final.log`。

未改任何安全数值、租约默认值或实验启用入口。完整 Linux/GStreamer
编译仍交主线程 PR24 CI；真实台架、执行机构与物理停车验证由主实施任务
协调。本机测试不构成物理停车完成证明。
