#!/bin/bash
set -euo pipefail
# All interfaces and routes belong to a disposable user/network namespace.
# No host NetworkManager, VPN, or physical interface settings are changed.
if [[ ${1:-} != --inside-namespace ]]; then
  exec unshare -Urn bash "$0" --inside-namespace
fi
ip link set lo up
unshare -n bash -c 'ip link set lo up; exec sleep 60' &
server_pid=$!
server_task=
cleanup() { if [[ -n "$server_task" ]]; then kill "$server_task" 2>/dev/null || true; fi; kill "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT
ip link add cell0 type veth peer name cellpeer
ip link add alt0 type veth peer name altpeer
ip link set cellpeer netns "$server_pid"
ip link set altpeer netns "$server_pid"
ip addr add 10.130.1.1/24 dev cell0
ip addr add 10.130.2.1/24 dev alt0
ip link set cell0 up
ip link set alt0 up
nsenter -t "$server_pid" -n ip addr add 10.130.1.2/24 dev cellpeer
nsenter -t "$server_pid" -n ip addr add 10.130.2.2/24 dev altpeer
nsenter -t "$server_pid" -n ip link set cellpeer up
nsenter -t "$server_pid" -n ip link set altpeer up
nsenter -t "$server_pid" -n ip addr add 203.0.113.8/32 dev lo
ip route add default via 10.130.2.2 dev alt0
nsenter -t "$server_pid" -n python3 -u -c 'import socket
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(("203.0.113.8",34567));s.listen(4);s.settimeout(15)
for _ in range(2):
 c,a=s.accept();c.sendall(a[0].encode());c.close()
s.close()' &
server_task=$!
sleep 0.3
python3 - <<'PY'
import socket,subprocess

def check(interface,source):
 s=socket.socket();s.settimeout(3)
 if interface:
  s.setsockopt(socket.SOL_SOCKET,socket.SO_BINDTODEVICE,interface.encode()+b'\0')
  assert s.getsockopt(socket.SOL_SOCKET,socket.SO_BINDTODEVICE,64).rstrip(b'\0').decode()==interface
 s.connect(('203.0.113.8',34567));seen=s.recv(64).decode();s.close();assert seen==source,(seen,source)
 print('interface',interface or 'default','peer_observed_source',seen)
check(None,'10.130.2.1')
check('cell0','10.130.1.1')
subprocess.run(['ip','link','set','cell0','down'],check=True)
s=socket.socket();s.settimeout(2);s.setsockopt(socket.SOL_SOCKET,socket.SO_BINDTODEVICE,b'cell0\0')
try:
 s.connect(('203.0.113.8',34567));raise AssertionError('bound socket escaped to alternate interface')
except OSError as e:
 print('bound_interface_down_failed_closed',type(e).__name__,e.errno)
finally:s.close()
PY
wait "$server_task"
