# Signaling TLS entry

`Caddyfile` is the public TLS/WSS entry for the C++ signaling backend. Keep
`mine-teleop-signaling-server` on `127.0.0.1:8765`; expose only Caddy port 443.
The template routes `/api/*`, the native API paths, and `/signaling/*`. Every
other path returns 404, so this endpoint does not publish the driving page.

Set the public origin and upstream before starting the pinned container:

```bash
export MINE_TELEOP_PUBLIC_ORIGIN=https://teleop.example.com
export MINE_TELEOP_SIGNALING_UPSTREAM=127.0.0.1:8765
docker compose -f deployments/caddy/compose.yaml up -d
```

Caddy uses Linux host networking so it can reach the backend's loopback socket;
the compose template is for the Linux server, not Docker Desktop on macOS.
Caddy is the only process that binds the public 443 socket.

The backend's default trusted proxy list is exactly `127.0.0.1,::1`. Keep that
single-hop boundary: Caddy's default reverse-proxy behavior ignores untrusted
incoming `X-Forwarded-*` values and sets the client source for the upstream.
Do not add public clients or broad subnets to the backend trusted list. Adding a
CDN/load balancer in front of Caddy requires a separately reviewed proxy-chain
configuration. See the official [Caddy reverse_proxy documentation](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy).

Caddy must obtain a certificate trusted by the target Mac/browser. Confirm
certificate renewal, HTTPS `/health`, WSS Upgrade, and the external port scan
before field use. `/admin/*` is deliberately absent from the public matcher;
admin operations require a separate private management path.

`Caddyfile.local-wss` uses Caddy's internal CA and fixed upstream port 18765.
It exists only for local integration tests. Export its root certificate and
pass it through `CURL_CA_BUNDLE`; never use this internal CA as public-deployment
evidence.
