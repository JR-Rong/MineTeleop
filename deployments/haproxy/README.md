# Port 6000 TLS/TURN multiplexer

Some vehicle networks can reach cloud TCP ports 443 and 6000 but block TURN's
standard 3478 port. HAProxy keeps the existing signaling endpoints and adds
plain TURN-over-TCP on the same public ports:

- port 443 TLS ClientHello -> Caddy at `127.0.0.1:14443`
- port 6000 TLS ClientHello -> Caddy at `127.0.0.1:16000`
- non-TLS TURN/TCP -> coturn's private PROXY-protocol listener at
  `172.24.29.133:3479`

Caddy still terminates and verifies the original
`teleop-field.internal:6000` client hostname; only its local listener moves to
16000. Coturn continues to advertise the cloud public address in relay
candidates.

The TURN backend must use HAProxy's binary PROXY protocol so coturn sees each
real client address instead of treating every allocation as a local proxy
connection. Configure coturn with `tcp-proxy-port=3479`, keep 3479 private, and
keep HAProxy's `send-proxy-v2` on the backend server line.

The frontend classifies traffic as soon as the first byte arrives. A TLS
handshake record (`0x16`) goes to Caddy; every TURN/STUN record goes to coturn.
Do not wait for the full `inspect-delay` before accepting non-TLS traffic:
WebRTC clients may abandon the initial TURN 401 challenge before a five-second
buffering delay expires.

Validate before restart:

```bash
haproxy -c -f /etc/haproxy/haproxy.cfg
caddy validate --config /etc/caddy/Caddyfile
```

Acceptance requires both:

```bash
curl --resolve teleop-field.internal:6000:60.205.213.254 \
  --cacert mine-teleop-field-root.crt \
  https://teleop-field.internal:6000/health

nc -vz 60.205.213.254 6000
```

The signaling server should issue
`turn:60-205-213-254.sslip.io:443?transport=tcp` for browsers and may retain
the port 6000 fallback for native clients. Do not publish the old
`turn:127.0.0.1:13478` SSH-forward URL. Chromium treats TCP 6000 as an unsafe
web port, so successful native TURN checks on 6000 are not browser acceptance.
