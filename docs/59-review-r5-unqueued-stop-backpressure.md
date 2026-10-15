# 审查 R5：只重取明确未发送的当前 STOP

基于冻结 `b141f8c`。用户已明确批准“按照你的建议来”：只有缓冲检查
明确超阈值、尚未调用 `send` 的 STOP，才可在原100ms发布窗口内通过
既有25ms tick重取。此提交落实该项授权，不开启实验，不改变任何安全阈值。
此前补充说明因执行通道断开未落盘，现以本文补齐；没有修改测试线f9013a3。

## 反例及可见后果

这是浏览器本地 DC 发送缓冲背压，不是云WSS排队、ICE未连接或车端未ACK。
实际页面设置 `bufferedAmountLowThreshold=1024`。修复前，在缓冲1025时
回调没有调用send，但courier已提前推进lastSeen；25ms后缓冲为0，原帧
仍剩75ms，下一次请求却携带last_seen_seq=1，不能再取原帧，实际入队0。

“消费”仅指页面获取游标推进，不等于native删除槽位、车收包或物理停车。
它浪费该帧的DC备用投递机会，须等新原生发布或其他既有路径；不能据此
声称所有STOP丢失。评审旧脚本用阈值0，本回归从真实页面源码提取阈值
与整个发送回调，以1024/1025复现，避免该测试捷径。

`r5-before.log` 保存冻结生产代码的预期失败：
`COUNTEREXAMPLE R5: skipped send consumed the still-valid original STOP`。
原courier、server源码副本和散列见工作目录的 `r5-backpressure-handoff.json`；该测试只
改fixture，不改生产代码，先失败后修复。

## 最小实现

本地导出一个不可变、按对象身份比较的 `emergencyBackpressure` 标记。
实际发送回调仅在channel open、buffer/threshold均为有限非负数且
buffer>threshold时返回该标记；该分支尚未调用send。成功调用send后
返回true；通道关闭/未知数据返回undefined；send抛错继续上报既有诊断。

courier仍先将帧按终止消费处理，仍保留所有原有效期、scope、归一化门禁。
只有回调返回同一个标记对象才恢复原游标，下次既有tick重新询问native。
false、undefined、数字、同形对象、Promise和抛错均不允许重取。已成功
入队的帧不因缺ACK重发；未加入缓存帧、队列、线程、定时器或应用层重传。

重取的是当前native publication：frame_json、seq、intent_seq、UTC与MAC
不变，服务端重新返回的valid_for_ms只为原窗口剩余量，不是新100ms。
保留HTTP往返耗时的既有保守检查。新native帧替代旧帧，不补旧帧；原帧
到期就不能再取，不重签、不续期。普通驾驶courier完全不变。

## 去重、撤权与复位

每次请求重新经过native session/generation、页面owner/ui epoch及media
attempt门禁。旧HTTP响应不能跨scope发送，native不再提供帧时不从浏览器
缓存绕过。原authority/reset校验、MAC和双路固定缓存去重没有变化；同一
STOP经DC/WSS只执行一次，过旧seq、冲突MAC和retired reset/authority仍拒绝。

特别保留既有STOP专用语义：`DirectAuthorityLease::reference()`在短租约
失效/撤权后仍可能保留原引用；经认证的归一化STOP本来可以只刹停，不能
由此发送普通运动。R5不新增短租约有效性门禁，也不新增撤权后的运动宽限，
不把STOP取帧或重复结果当作租约/看门狗刷新。authority/reset变化的旧帧
仍由原native/receiver边界拒绝。本地复位后的迟到旧成功仅保留历史，沿用R4。

用户可见变化仅限已启用实验且瞬时背压解除仍在原窗口内时，同一STOP能
获得一次真正入队机会；不改变按键/急停含义、松开重按、物理确认标准或
默认配置。600ms仍仅localhost fixture，生产短租期未因本项获批准。

## 验收与证据范围

`node cpp/tests/control_transport.test.cjs --r5-only` 运行6组新增回归；完整
courier为19组，页面控制逻辑为98组。新增fixture提取当前生产回调：

| 场景 | 断言 |
| --- | --- |
| 缓冲1025，25ms后归零 | 未入队时游标不推进；同一原字节入队一次，缺ACK不重发 |
| 缓冲1024 | 可入队且仅一次；同形标记/未知返回/抛错不重取 |
| 背压到100ms、99ms取帧但HTTP到100ms | 原帧不发送，不延寿、不重新发布 |
| seq1待投递时native发布seq2 | 仅发送当前seq2，不回补seq1 |
| session/generation/owner/ui epoch/attempt变化或native禁用 | 旧HTTP及旧native帧不泄漏；仍单一在途请求 |
| channel关闭、缓冲/阈值未知、无效数值 | 按终止消费，不推测为可重试背压 |

现有localhost集成的blocked-WSS STOP用例新增实际native endpoint断言：
25ms后不推进last_seen可取完全相同签名字节，剩余期减少；同字节经DC
再WSS返回同一缓存结果、result序号不增长；原发布过100ms后为空，新
明确STOP产生更高seq，旧帧不再返回。专门R4 localhost目标仍验证复位后
260ms迟到旧结果不确认当前锁存。生产receiver4组仍覆盖MAC、scope、
authority/reset、双路去重及有界缓存。各项实际运行结果以最终handoff为准。

JS只模拟DC缓冲和native响应；localhost用真实native/cloud/common service
和签名字节，DC传递为模拟。均不证明真实SCTP拥塞、P2P/NAT、硬件20Hz或
物理停车。Linux/GStreamer整程序编译、测试线冻结轮次及跨机验收由主线程协调。

本机最终纯逻辑/源码提取CTest **12/12通过，6.43秒**，localhost CTest
**5/5通过，16.16秒**；courier **19项**、页面逻辑 **98项**通过。
构建命令首次误把authority-cutoff的CTest别名当构建目标，已保留失败日志；
真实修改目标已编译完成，纠正目标后的其余构建通过，没有为此改源码。

## 集成到f9013a3的固定保留清单

共同基线2cae265、目标f9013a3、直连b141f8c的只读比较，main增量13文件，
共同修改仅 `CMakeLists.txt` 和 `cpp/src/webrtc_media.cpp`。字节副本的
三方预览两者文本冲突0；这不保证逐提交cherry-pick或运行语义无冲突。
R5只改server页面回调、courier及测试/文档，不修改这两个共同文件。
外部 `r5-decision-b141f8c/integration-overlap-preview.json` 保存blob散列。

- CMake必须同时保留f901真实CLI校时/录像EOS测试及直连所有目标和平台条件。
- webrtc_media必须保留生产RecordingTap/stop_recording_pipeline真实EOS排空
  接入、控制先关闭的顺序，并保留直连worker、R1撤权、R2异常关闭及R3缓存。
- `cpp/apps/mine_teleop.cpp` 保留仅http_failure校时异常按原1秒退避；401/
  非法样本仍拒绝，500ms请求预算不变。
- `recording_pipeline.hpp`、`recording_gstreamer.cpp` 保留真实tap EOS后NULL，
  共享ERROR不能替代EOS，原3秒/总8秒预算不变。
- `recording.cpp` 保留父失败不复用旧summary_persisted证明。
- `session_logs.cpp` 保留producer整数reported时间、service-instance先验、
  实际attempt锚、rotation和有界缓存；不退回launcher logged_at授权，不让
  失败日志提升EOS完整性，不扩大旧会话读取范围。
- `media_time_sync_retry_loopback.py`及录像/日志测试保留真实CLI、真实EOS、
  缺EOS、父失败、SID重用和producer时间边界；非法早期fixture不作有效red。
- `native_control_ack_tests.cpp`保留f901真实满128容量、六次精确ACK及单连接/
  同generation/零失败前提；不退回固定100Hz五秒必填满的假设。
- `control_tests.cpp`保留22c59ab拒绝reason与单调时序、f901旧响应gate/四个
  有界独立client/应用处重叠见证；原200ms/230ms断言不放宽。

主线程在测试线完成f901冻结轮次后协调集成；不整文件覆盖、不偷换现场
版本、不推送部署。媒体HTTP失败与控制生命周期耦合仍是已知未解决项。
