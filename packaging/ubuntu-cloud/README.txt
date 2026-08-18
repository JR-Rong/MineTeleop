Mine Teleop Cloud for Ubuntu 22.04 x64
======================================

This package contains the native signaling server, its Ubuntu 22.04 shared
library closure, systemd units, Caddy/HAProxy/coturn configuration assets, the
deployment script, and additive driver/vehicle management scripts. It contains
no credentials.

The deployment target must be Ubuntu 22.04 x86_64 with systemd. Run:

  sudo ./deploy-cloud.sh \
    --signaling-config /secure/staging/signaling-server.yaml \
    --identity-secrets-dir /secure/staging/identity-secrets \
    --turn-secret-file /secure/staging/turn-static-auth.secret \
    --turn-realm 60-205-213-254.sslip.io \
    --turn-host 60-205-213-254.sslip.io \
    --caddy-config deployments/caddy/Caddyfile.three-machine \
    --haproxy-config deployments/haproxy/haproxy.three-machine.cfg

The bundled three-machine proxy files describe the repository's current field
topology, including fixed public/private addresses. Inspect and edit copies
before using them on a different server.

The first deployment installs caddy, coturn, curl, and haproxy, stores the
application under /opt/mine-teleop, installs protected configuration under
/etc/mine-teleop, enables mine-teleop-cloud.target, and checks the loopback
health endpoint. Existing /etc/mine-teleop files are preserved unless a
replacement is explicitly passed. Replaced files and the previous application
directory are backed up.

For a binary/unit-only first step:

  sudo ./deploy-cloud.sh --no-start

Then run /opt/mine-teleop/deploy-cloud.sh with the configuration arguments.
For later application-only upgrades, existing validated configuration is
reused:

  sudo ./deploy-cloud.sh

Use --skip-package-install only when caddy, coturn, curl, haproxy, openssl,
python3-yaml, and util-linux are already installed. See ./deploy-cloud.sh --help
for all options.

To add a driver without restarting signaling or the other cloud services:

  sudo /opt/mine-teleop/add-driver.sh \
    --id driver-console-003 \
    --config /etc/mine-teleop/signaling-server.yaml \
    --vehicles vehicle-001

To add a vehicle and grant it to an existing driver without restarting:

  sudo /opt/mine-teleop/add-vehicle.sh \
    --id vehicle-003 \
    --config /etc/mine-teleop/signaling-server.yaml \
    --assign-to-driver driver-console-001

The scripts lock, back up, and validate the identity YAML, create protected
random secret files without printing their contents, and commit only additive
identity updates. The running signaling service discovers a driver on its first
matching login and a vehicle on its first matching connection. Deletion,
existing-secret rotation, and permission reduction still require a validated
maintenance-window restart.
