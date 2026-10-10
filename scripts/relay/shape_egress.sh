#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 3 || "$1" != '--apply' ]]; then
  printf 'Usage: %s --apply PUBLIC_INTERFACE COTURN_UID\nApply only during the relay maintenance window; replaces this interface qdisc.\n' "$0" >&2
  exit 2
fi
interface="$2"
relay_uid="$3"
[[ "$interface" =~ ^[A-Za-z0-9_.:-]+$ && "$relay_uid" =~ ^[0-9]+$ ]] || exit 2
ip link show dev "$interface" >/dev/null
# Socket marks set by HAProxy remain intact. Coturn is a dedicated process UID;
# shared 443/6000 traffic is never classified merely by port.
nft list table inet mine_teleop_relay >/dev/null 2>&1 && nft delete table inet mine_teleop_relay
nft -f - <<EOF
table inet mine_teleop_relay {
 chain output {
  type route hook output priority mangle; policy accept;
  meta skuid $relay_uid meta mark set 0x20
 }
}
EOF
tc qdisc replace dev "$interface" root handle 1: htb default 10
tc class replace dev "$interface" parent 1: classid 1:1 htb rate 10mbit ceil 10mbit
tc class replace dev "$interface" parent 1:1 classid 1:10 htb rate 2mbit ceil 10mbit prio 0
tc class replace dev "$interface" parent 1:1 classid 1:20 htb rate 8mbit ceil 8mbit prio 1
tc qdisc replace dev "$interface" parent 1:10 handle 10: fq_codel
tc qdisc replace dev "$interface" parent 1:20 handle 20: fq_codel
tc filter replace dev "$interface" parent 1: protocol all prio 10 handle 0x20 fw flowid 1:20
tc -j -s qdisc show dev "$interface"
