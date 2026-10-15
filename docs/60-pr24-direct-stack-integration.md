# PR24 直连实验栈与f901修复集成

集成起点是远端实际核对的 `f9013a3bef946ef81278f05bbc71769d442329f8`；
另一路为16提交直连栈 `eec6a77c2b473c1b7b339537e28f35c125f6e260`。
本地合并保留两条完整历史。实验仍默认0/false，无YAML/CLI/UI启用入口，
600ms仍仅测试fixture；没有部署、切换测试线运行包或合并GitHub PR。

## f901修复保留

两个共同修改文件CMakeLists/webrtc_media采用三方自动合并，未整文件覆盖。
保留校时包装传输错误原1秒重试、真实RecordingTap EOS排空和控制先关闭
顺序、父终态失败持久化证明失效，以及producer时间/实际attempt/服务实例
隔离的晚到录像证据。ACK128及HTTP旧响应gate、实际重叠见证和200/230ms
断言保留；关键相机、控制年龄、看门狗和恢复期限均未放宽。

集成前11个非重叠main文件逐字节保留。随后只为下面静态合约适配修改
control_tests.cpp的4个架构/源码文本检查函数；其余51个test_函数及完整
native_control_ack_tests.cpp与f901逐字节一致，运行时HTTP修复没有改动。

## 旧静态合约与新架构

首次本机完整CTest为41/43通过：control-tests的50个用例通过，4个旧文本
检查失败；runtime-diagnostics的旧DC条件检查失败。它们要求所有普通DC
无条件拒绝、管理仅走浏览器DC和云断开立即清权，未描述已批准的有界直连。
原始失败日志完整保留，没有删除用例或放宽断言时间。

更新合约仍检查：旧浏览器控制在parse/apply前拒绝；DC的旧channel指针
静默退出先于状态发布；新原生DC必须MAC认证，短授权及长期截止/transport
epoch Fence拒绝先于共同服务执行。参数和VCU connect仍须当前认证云投递，
不能借短暂云失联重新建权。页面仅在native明确给出continuation时保留
既有会话，非该情况仍清输入并禁止新建。普通控制仍单一选中路径。

在源码副本分别去掉旧channel、旧浏览器拒绝、MAC和Fence拒绝门禁，四个
负例均失败。该检查是静态保护，不冒充实际线程或物理执行验证。

## 本机验证和CI边界

独立工作树从已有锁定JSON/yaml源码、FetchContent完全离线模式构建，
vehicle runtime关闭。固定Python/JS套件通过，六项依赖打包后二进制的
desktop生命周期用例明确跳过，不算通过。完整本机43项CTest复核通过，
包含原控制54项、ACK128、录像传输、session logs以及全部新增直连目标。

本机结果对应构建时的精确集成源码树，不作为可部署包；完整Linux/
GStreamer与六平台以提交后同head CI为准。每次CI的SHA、运行/产物记录和
原始日志保存在工作目录集成交接清单。真实SCTP/P2P/TURN、跨机20Hz、
CAN/VCU及物理停车尚待协调验收，媒体HTTP/控制生命周期耦合仍未解决。

测试线继续使用原f901冻结包；新版本仅在该轮结束并由主线程安排后接包。

首轮769e4ad的Windows x64 CI在Json与std::string_view直接比较时报MSVC
重载歧义。后续修正显式读取JSON字符串再作身份/path比较；非字符串身份
仍报原scope invalid_argument，字段、签名、路径选择和时限不变。该轮
Linux/GStreamer整程序编译已通过；Windows红灯及后续同head CI分别保存，
不能将前一提交的编译/测试结果归给后一提交。
