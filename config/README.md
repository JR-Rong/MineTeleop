# App 登录密码

云端通过 `auth.mobile_app_password_file` 读取统一 App 密码；默认路径为 identity YAML 所在目录的 `secrets/app-token`，生产环境为 `/etc/mine-teleop/secrets/app-token`。文件内容即手机登录密码，无用户名、无 JSON/YAML 包装，末尾换行会被去掉。

密码文件不得提交；部署时单独配置，让云端运行用户可读，建议权限 `600`。修改后重启信令服务生效。只要有车辆开启 `mobile_approval_required`，文件缺失或为空就会拒绝启动。

这里的 `config/app-token` 是旧版路径。云端部署脚本会在替换应用目录前迁移到默认持久路径，并优先保留已有持久密码。手工换包须自行迁移。YAML 示例仍在仓库的 `configs/`；使用审批开发示例时默认密码为 `configs/secrets/app-token`，已加入 Git 忽略规则。
