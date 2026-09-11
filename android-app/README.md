# 手机授权 · Android

原生 Java / Android Views App，支持 Android 8.0+。只输入统一 App 密码登录；云端从工作目录的 `config/app-token` 读取该密码。没有用户名，也没有内嵌网页。

## 构建与安装

安装 JDK 17、Android SDK Platform 35 和 Build Tools。设置 `ANDROID_HOME`（或在忽略的 `local.properties` 中设置 `sdk.dir`），然后：

```bash
cd android-app
./gradlew assembleDebug testDebugUnitTest lintDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

`app-debug.apk` 是可直接安装的调试签名包。正式发行需要使用团队持有的签名密钥配置 release 签名；仓库不包含生产签名密钥。

默认连接服务器 `60.205.213.254` 的 HTTPS 入口 `https://60-205-213-254.nip.io:6000`，登录页直接显示当前地址。只需输入 `config/app-token` 中的密码并开始值守。已有安装会保留手动保存的地址；可在“连接设置”点击“恢复默认”再保存。自定义地址不包含 `/mobile/` 路径。云端配置参考 `../docs/28-mobile-control-approval.md`。

默认校验证书且禁止明文 HTTP。仅 debug 版本允许 `localhost`、`127.0.0.1` 和 Android 模拟器宿主 `10.0.2.2` 的 HTTP，便于本地联调。例如模拟器设置 `http://10.0.2.2:18779`。release 始终要求 HTTPS；不包含跳过证书验证的开关。

## 使用

1. 登录并允许通知权限，App 启动值守前台服务。
2. 控制端点击一次连接，需要审批的申请会自动进入列表并产生通知。
3. 核对车辆、驾驶员和期限，点击同意或拒绝；同意后控制端自动连接。
4. 在 App 内点击“退出值守”停止服务并注销当前手机令牌。

只有云端地址存储在本机。密码和会话令牌不持久化，App 进程被杀死后需要重新登录。不要把关闭页面等同于退出值守；正常切到后台时前台服务继续检查。

值守服务每 3 秒查询云端，采用用户主动开启的 `specialUse` 前台服务并显示常驻通知。没有接入 Firebase 或厂商推送。系统休眠、厂商省电策略、断网和强行停止 App 可能使提醒延迟或停止；需要在目标设备验收。通知只打开审批台，不提供锁屏一键批准。

单元测试覆盖 HTTPS 地址边界和申请到期禁用；lint 检查最低系统版本、权限与资源。云端 HTTP/WSS 授权、并发与自动连接由 C++ 测试覆盖。上线前还需要验证目标手机安装、通知、后台值守与真实云端证书。
