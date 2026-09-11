# App 登录密码

云端进程从工作目录的 `config/app-token` 读取统一 App 密码。文件内容即手机登录密码，无用户名、无 JSON/YAML 包装，末尾换行会被去掉。

`app-token` 已加入 Git 忽略规则；部署时单独配置，让云端运行用户可读，建议权限 `600`。修改后重启信令服务生效。只要有车辆开启 `mobile_approval_required`，文件缺失或为空就会拒绝启动。

这里的 `config/` 是专门的 App 密码目录；车辆和驾驶员 YAML 示例仍在仓库原有的 `configs/`。
