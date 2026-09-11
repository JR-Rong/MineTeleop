# 手机审批控制权

云端按车辆选择是否要求手机审批。审批发生在创建控制会话、签发控制令牌之前；开启后，任何控制端调用 `POST /sessions` 都必须经过门禁。App 使用独立于驾驶员的统一密码登录，可处理所有开启手机审批的车辆。

驾驶员只需点击一次连接：云端检查车辆配置，不需要审批则立即建立会话；需要审批则转交手机，同意后自动建立会话，拒绝或超时则结束本次申请。车端不变；控制端仅增加连接等待、取消与状态提示，保留原来的媒体、控制和 VCU 握手流程。此门禁覆盖新控制会话，不能逐条审批已经建立的 WebRTC DataChannel 内部 VCU 握手命令。

现有控制端到云端的 HTTP 请求超时为 5 秒，因此不长期挂起云端请求。控制端的一次 `connect()` 调用在后台每 500 毫秒查询同一 `approval_request_id`，手机同意后云端签发会话，控制端自动继续 WSS / WebRTC 信令。浏览器只发起一次本地 `/api/connect` 请求，不需要再次点击。旧控制端仍只能在同意后手动重试，**上线自动连接功能必须同步更新控制端程序**。

## 云端配置

参考 `configs/signaling-server.mobile-approval.dev.yaml`，在正在使用的 identity YAML 中增加：

```yaml
auth:
  mobile_approval_timeout_ms: 120000
  drivers:
    - id: driver-console-001
      password_file: /etc/mine-teleop/secrets/driver-console-001.password
      vehicles: [vehicle-001]
  vehicles:
    - id: vehicle-001
      device_token_file: /etc/mine-teleop/secrets/vehicle-001.token
      mobile_approval_required: true
```

- `mobile_approval_required` 默认 `false`；只有明确配置 `true` 的车辆需要审批。
- App 没有用户名。统一密码从**云端进程工作目录下的 `config/app-token`** 读取，文件内容就是手机登录密码；允许末尾换行，其他字符原样保留。不是 `configs/app-token`，不从 YAML 或环境变量读取 App 密码。
- 只要有车辆开启审批，文件缺失、不可读或内容为空就拒绝启动；未开启任何审批车辆时不要求此文件。
- 仓库已忽略 `config/app-token`，不要提交密码。部署时单独创建该文件，让云端服务用户可读（建议权限 `600`），并确认 systemd 的 `WorkingDirectory`。密码修改后重启云端生效，原手机登录和申请全部失效。
- 手机登录后可以审批所有开启门禁的车辆。多部手机可同时登录，首次决定生效；相反决定或另一部手机重复决定被拒绝。退出只注销当前手机。
- `mobile_approval_timeout_ms` 默认 120 秒，允许 1–600000 毫秒。等待和批准共用同一个期限，同意不延期。
- 手机登录有效期沿用 `--driver-token-ttl-ms`（默认 30 分钟），但令牌与驾驶员身份完全分离。审批密码错误锁定独立于驾驶员登录，并保留现有来源请求限流。

在云端安全维护窗口更新 YAML，先运行 `mine-teleop-signaling-server --config <配置路径> --validate-config`（实际参数见 `--help`），再按现有部署流程替换程序、更新 Caddy 配置并重启信令服务。此版本按启动配置加载，不提供运行时配置界面或热更新。重启会结束原有会话；仅在车辆安全停稳、会话退出后操作。本文不自动部署。

## 安卓 App

原生 Android 项目位于 `android-app/`，支持 Android 8.0（API 26）及以上。构建与安装见 `android-app/README.md`。

App 默认连接 `https://60-205-213-254.nip.io:6000`（服务器 `60.205.213.254`），登录页直接显示当前地址。“连接设置”可修改或恢复默认；已有安装保留手动保存的地址。登录页**只输入 `config/app-token` 中的密码**，点击“登录并开始值守”。申请显示车辆、驾驶员、倒计时及“同意 / 拒绝”；同意后控制端自动连接。

值守期间由用户主动启动的 Android 前台服务每 3 秒拉取申请，显示常驻值守通知；新申请产生系统通知，点击进入 App 核对后决定。需要允许通知权限。本版无 Firebase / 厂商推送依赖；休眠、省电策略、断网及强行停止 App 仍可能延迟或阻止提醒，需在目标手机验证。App 被系统杀死或登录过期后须重新输入密码，不自动恢复授权值守。

App 只保存云端地址，不保存密码或登录令牌。密码清空输入框，令牌只在服务内存。网络失败时禁用审批，服务器仍检查请求期限与连接代次；提交响应丢失时重新同步状态，不直接显示成功。云端注销响应丢失时会停止本机值守并明确提示，云端令牌以其原有效期失效。

等待期间控制端显示“等待手机确认”，后台请求维持驾驶员在线；点击“安全退出”或关闭页面会取消等待。页面关闭的取消请求若未送达，依靠云端连接 / 令牌过期清理，不保证故障情况下瞬时撤回。

`https://<云端域名>/mobile/` 保留为备用网页入口，同样只有密码登录；网页需要保持打开，不具备安卓前台服务能力。

## 请求生命周期与接口

1. 驾驶员申请连接。云端验证驾驶员、车辆权限、在线状态和已有控制权。
2. 需要审批时创建随机 `request_id`，绑定驾驶员登录代次与车辆连接代次；不生成 `session_id` 或 `control_token`。
3. 同一申请的连接重试返回原 `request_id`；同车其他驾驶员收到 busy，不能覆盖申请。
4. 手机取得列表并提交决定。批准只授权这次申请；拒绝一直生效到原申请期限结束。
5. 控制端后台对同一申请自动等待；云端在原期限内消费批准一次，签发控制令牌，随后建立原有信令与媒体联系。拒绝、超时或申请代次变化立即终止等待，不自动创建另一条申请。
6. 申请过期、驾驶员退出/掉线/重新登录、车辆离线/重新注册/吊销，都会使等待或批准失效。云端重启清空申请与手机登录。
7. 安全退出和结束会话会唤醒等待任务，撤销其审批申请。云端取消接口也能关闭已签发但控制端未确认收到的对应会话，避免迟到响应遗留控制权。取消与连接建立串行完成；浏览器忽略退出后的旧连接结果。
8. 已建立会话仍使用原来的控制租约、续期与安全退出机制；同一会话续期无需重新审批。

| 接口 | 身份 | 用途 |
|---|---|---|
| `POST /mobile/api/login` | `password` | 返回审批专用 `token` |
| `GET /mobile/api/requests` | 审批令牌请求头 | 获取所有需手机确认车辆的最新申请 |
| `POST /mobile/api/requests/{request_id}/decision` | 审批令牌请求头 | `{"decision":"approve"}` 或 `{"decision":"reject"}` |
| `POST /mobile/api/logout` | 审批令牌请求头 | 使当前令牌失效 |
| `POST /sessions/approval/cancel` | 驾驶员 `driver_id`, `token` | 通过 `vehicle_id`, `approval_request_id` 取消指定申请及其会话 |

审批令牌请求头为 `X-Mine-Teleop-Approver-Token`，禁止使用 URL 参数认证。所有手机接口返回 `Cache-Control: no-store`。App 密码失败锁定及来源限流复用云端机制；新请求、决定、消费和失效写入现有审计链路，审计写入失败不能批准或发放控制权。

`POST /sessions` 的审批响应使用 HTTP 409，`issue_code` 包含 `mobile_approval_pending`、`mobile_approval_busy`、`mobile_approval_rejected`、`mobile_approval_ended`、`mobile_approval_stale`。后台等待仅处理 pending，其余结果均终止；`approval_request_id` 指定后，过期或不匹配不能生成新申请。申请状态可能是 `pending`、`approved`、`rejected`、`expired`、`cancelled`、`consumed`。手机只保留每辆车最新一次申请；历史记录以审计日志为准。

## 验证

```bash
cmake -S . -B /tmp/mine-teleop-mobile-build \
  -DMINE_TELEOP_BUILD_TESTS=ON \
  -DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=OFF \
  -DMINE_TELEOP_FETCH_MISSING_DEPS=ON
cmake --build /tmp/mine-teleop-mobile-build -j 6
ctest --test-dir /tmp/mine-teleop-mobile-build --output-on-failure
```

`mine-teleop-mobile-approval-tests` 覆盖默认放行、无批准拦截、请求去重、一次性消费、竞争决定、拒绝/超时、身份用途隔离、审批车辆范围、连接代次变更、吊销、审计失败、登录限流、静态资源，以及控制端一次调用自动建立 HTTP/WSS 会话、拒绝/超时/退出结束等待和迟到授权响应的清理。软件测试不等同于现场验收；部署后还需用实际两部手机、目标控制端和安全停稳的车辆验证 HTTPS、后台行为、在线审批与断线恢复。

## 自动连接版本验证范围

自动连接测试通过真实控制端运行时和本地 HTTP/WSS 服务，覆盖同意、拒绝、超时、安全退出、结束会话、车辆连接被替换；另有固定申请编号重试、已签发会话取消、授权响应与退出竞争测试。等待期间不创建活动会话，结束后不遗留待审批状态。

结构复核：复用现有 HTTP 状态错误、连接代次、审计与租约；只增加连接生命周期同步，不更改驾驶命令和车端；等待只处理本次 pending，500 毫秒间隔避免忙轮询，退出唤醒等待，不等待整段超时。

尚未执行生产部署和实车控制验收。安卓构建、单元测试及静态检查不替代目标手机安装、锁屏 / 省电 / 断网与现场流程验收。
