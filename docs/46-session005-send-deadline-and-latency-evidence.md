# Session005发送期限与分开的链路证据

本轮依据2026-10-15两个完整会话键的本机只读分析：
`service-8f5fa6cf8a43260479925201_session-000016`与
`service-6804f480eea33cdb08fe2dc7_session-000005`。没有部署、车辆操作或网络配置变更。

## 不能合并的触发链

- 016有45个已接受输入刷新间隔超过200ms，页面timer lag最大2116.7ms。
  正常定时器、两个并行普通请求、150ms响应期限的离线队列在0–220ms
  响应及不返回响应时，最大派发间隔66–84ms；注入2116.7ms页面停顿才
  产生2140.7ms间隙。真实输入存储按200ms失效，旧长按不能复活，归零
  重按可恢复。它不定位页面停顿来自解码、渲染、系统还是其他任务。
- 016同一ACK drain的976..980包龄499/437/374/309/246ms，978触发第三个
  异常，981的183ms新ACK同毫秒恢复。完整生产drain的离线提取重放与
  原生回归保留5个bad revision、1个brake revision及归零要求。
  按最新ACK覆盖这段历史会改变既有三异常语义，本轮没有这样做。
- 005没有超过200ms的已接受刷新间隙，timer lag最大89.6ms；只有两个
  过龄入口ACK，连续最多2个。最后有效命令3927在13:31:59.389 UTC，
  3928..3930过龄，13:32:01.426因2037ms无有效命令硬停并撤销profile。
  13:32:22.044–39.170的发送失败在硬停之后，不能解释第一次硬停。

## 发送期限的确证缺口与修复

旧`send_all`只在`CURLE_AGAIN`分支检查期限。当前方法的可控时钟重放
证明：20ms预算下，4次各30ms的成功短写会在120ms后返回成功；单次
成功调用用时17127ms也不会被拒绝。这证明调用方缺口，不证明现场
17127ms全部在同一个底层调用中。

Windows依赖锁为curl8.17.0-DEV/Schannel。锁定的vcpkg端口取上游
`curl-8_17_0`，其Schannel发送函数内部循环等待整个TLS记录，使用
libcurl自己的time-left。调用方的20ms参数没有自动传入这个内部等待。
这是实际源码支持的阻塞路径；现场ZIP没有整包BUILD-INFO或逐调用
计时，不能据页面SHA唯一确认现场依赖或断言唯一根因。

- 一次JSON序列化、WebSocket组帧、全部写入和可写等待共享一个绝对
  单调截止时间。每次调用前后均检查，成功短写或最后一次迟到成功
  不再逃逸；记录单次curl耗时、返回码、轮询、已报告字节和取消状态。
- 每个连接有一个休眠的期限观察器。它只对固定TCP端点的独立描述符
  执行`shutdown`，不调用curl、不释放curl handle。POSIX用close-on-exec
  duplicate，Windows用同进程WSADuplicateSocket/WSASocket；原号码被
  回收复用也不会转向另一连接。owner停止并join观察器后才释放原handle。
- 停止请求可中断当前发送；错误、超时或取消使连接退役。部分帧不能
  接着发送下一条控制/急停，也不在这个流上重放旧帧。既有重连、鉴权、
  急停独立通道和序号语义保留。清理前shutdown避免TLS close-notify
  在WebSocket close尝试之后另起网络等待。
- 发送失败也保留开始时间、socket锁实际持有时间及发送阶段。实际
  curl/TLS版本写入传输诊断，下一份日志可区分组帧、单次后端与外层等待。

20ms发送预算、250ms close预算和既有连接预算没有调大；200ms包龄/
输入、500ms ACK停滞、三异常、25ms默认校时以及现场2000ms硬停均不改。
这不是硬实时OS保证。观察器可以终止socket等待，无法抢占任意CPU/驱动
死循环；连接建立和其他HTTP worker各自仍有原有期限。

依据：[curl easy-send契约](https://curl.se/libcurl/c/curl_easy_send.html)、
[锁定vcpkg端口](https://github.com/microsoft/vcpkg/blob/c0898ec76e1d29dacda1937dbd327c5a396fc0f9/ports/curl/portfile.cmake)、
[Schannel发送等待](https://github.com/curl/curl/blob/curl-8_17_0/lib/vtls/schannel.c#L1842)、
[libcurl handle线程约束](https://curl.se/libcurl/c/threadsafe.html)、
[Windows共享socket描述符](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaduplicatesocketw)。

## 原因与指标

接收路径已经因包龄、输入或历史异常进入等待，但当前四项readiness
标志可能已恢复。外层观察器以前会把默认`critical_video`锁存为首因，
导致两份日志中没有failed critical lane的control_link/input/history被
误报相机故障。现在继承真实接收路径原因；真实critical lane故障和
当前time/link/input优先级保留，健康判断与制动输出不变。日志同时
保留`recovery_latched_cause`、`control_service_recovery_cause`和
`failed_camera_present`；没有失败相机时不再输出泛化相机名字。

页面分别显示媒体ICE RTT、云入口ACK包龄/收到后年龄/进度停滞、
关联车端软件结果往返及收到后年龄、最近软件接受seq及其年龄。
新鲜云ACK不能刷新车端接受记录；新拒绝不会抹掉或刷新最近接受
记录；会话/generation不符或没有关联时显示未知。媒体stats的
`control_rtt_ms`明确为null，实际ICE值只在`media_transport_rtt_ms`。
CAN反馈年龄仍单独显示，缺失年龄不转成0。以上均为显示，未增设
新的驾驶门禁，软件接受/收到ACK不等于VCU或物理执行。

旧缓存等待标志在页面写入与遥测处理的序号过滤不一致，已用实际
函数离线表征；但较新accepted零心跳也可能仍处制动等待，不能仅凭
accepted seq清掉保护。本轮保留该行为，未认定它是016按键清除根因。

## 离线验收与边界

- `mine-teleop-websocket-send-tests`：成功短写/单次迟到成功、AGAIN、
  取消可join、过期期限/旧timer、新连接所有权及POSIX强制FD复用。
- `mine-teleop-websocket-send-tls-tests`：临时证书、仅127.0.0.1真实TLS/WSS
  握手后对端停止读取；验证超时、失败流拒绝后续急停帧与owner清理。
  Windows打包明确构建并运行该目标，实际Schannel结果由该head CI记录。
- `mine-teleop-recovery-enforcer-tests`提取未改写的生产观察器与freshness
  方法，使用真实控制服务，覆盖三种非相机原因和真正关键相机对照。
- 原生输入回归重放真实ACK时序，JS回归覆盖2秒页面停顿与本地响应
  超时的区别；现有300ms健康窗口、释放重按、watchdog与急停回归保留。
- 车端结果窗口与实际页面回归覆盖云ACK持续但车端拒绝/接受过期、
  无关联、空年龄、会话/generation更换及媒体值不充当控制时延。

没有复现或修复网络/内核/TLS代理缓冲的唯一现场根因，没有启用实验
控制直连。摄像头2/4的真实V4L2故障、CAN64MiB导出预算缺失退出尾部
仍属独立问题；本轮没有声称这些问题、实车制动或最终物理停车已验收。
