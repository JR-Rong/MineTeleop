# Mine Teleop cloud systemd group

`mine-teleop-cloud.target` is the single lifecycle entry for the cloud stack.
It starts and stops four independent daemons:

- `mine-teleop-signaling-server.service`
- `mine-teleop-turn-server.service`
- `caddy.service`
- `haproxy.service`

Keeping separate services preserves failure isolation, restart policies, and
per-component logs. The target is operational grouping, not a process merge.

Install from a checkout on the cloud host:

```bash
sudo install -m 0644 \
  deployments/systemd/mine-teleop-signaling-server.service \
  deployments/systemd/mine-teleop-turn-server.service \
  deployments/systemd/mine-teleop-cloud.target \
  /etc/systemd/system/
sudo install -D -m 0644 \
  deployments/systemd/caddy.service.d/mine-teleop-cloud.conf \
  /etc/systemd/system/caddy.service.d/mine-teleop-cloud.conf
sudo install -D -m 0644 \
  deployments/systemd/haproxy.service.d/mine-teleop-cloud.conf \
  /etc/systemd/system/haproxy.service.d/mine-teleop-cloud.conf
sudo install -D -m 0644 \
  deployments/systemd/mine-teleop-signaling-server-field.override.conf \
  /etc/systemd/system/mine-teleop-signaling-server.service.d/field.conf
sudo systemctl daemon-reload
sudo systemctl enable --now mine-teleop-cloud.target
```

If the four units were previously enabled individually, they may remain
enabled without creating duplicate processes. To make the target the only boot
entry, remove the individual enablement without stopping the running units:

```bash
sudo systemctl disable \
  mine-teleop-signaling-server.service \
  mine-teleop-turn-server.service \
  caddy.service \
  haproxy.service
sudo systemctl enable mine-teleop-cloud.target
```

Operate and inspect:

```bash
sudo systemctl start mine-teleop-cloud.target
sudo systemctl restart mine-teleop-cloud.target
sudo systemctl stop mine-teleop-cloud.target
sudo systemctl --no-pager --full status \
  mine-teleop-cloud.target \
  mine-teleop-signaling-server.service \
  mine-teleop-turn-server.service \
  caddy.service \
  haproxy.service
```

Do not deploy repository example secrets. Validate the signaling identity
configuration, coturn secret, Caddy certificate path, and HAProxy configuration
before the first restart.
