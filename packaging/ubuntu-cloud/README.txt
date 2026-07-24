Mine Teleop Cloud for Ubuntu 22.04 x64
======================================

This package contains the native signaling server, its Ubuntu 22.04 shared
library closure, systemd units, Caddy/HAProxy/coturn configuration assets, and
one deployment script. It contains no credentials.

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

Use --skip-package-install only when caddy, coturn, curl, and haproxy are
already installed. See ./deploy-cloud.sh --help for all options.
