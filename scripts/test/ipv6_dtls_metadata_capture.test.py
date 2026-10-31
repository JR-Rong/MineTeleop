#!/usr/bin/env python3
import importlib.util
import ipaddress
import socket
import struct
from pathlib import Path

path = Path(__file__).with_name('ipv6_dtls_metadata_capture.py')
spec = importlib.util.spec_from_file_location('capture', path)
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
local = ipaddress.IPv6Address('2001:4860:4860::8888').packed
peer = ipaddress.IPv6Address('2606:4700:4700::1111').packed
record = b'\x17\xfe\xfd\x00\x01' + (123).to_bytes(6, 'big') + b'\x00\x04' + b'test'
udp = struct.pack('!HHHH', 1234, 5678, len(record) + 8, 0) + record
header = bytes([0x60, 0, 0, 0]) + struct.pack('!HBB', len(udp), 17, 64) + peer + local
p = header + udp
x = capture.classify(p, local, peer)
assert x == {'direction': 'in', 'src_port': 1234, 'dst_port': 5678, 'udp_bytes': len(udp),
             'dtls_type': 23, 'dtls_epoch': 1, 'dtls_record_seq': 123}
assert capture.classify(p, peer, local)['direction'] == 'out'
assert capture.classify(p[:52], local, peer) is None
assert capture.classify(p[:40] + udp[:8] + b'\x80' + udp[9:], local, peer) is None
assert capture.classify(p[:6] + b'\x2c' + p[7:], local, peer) is None
assert capture.kernel_timestamp_ns([(socket.SOL_SOCKET, capture.SO_TIMESTAMPNS, struct.pack('@ll', 10, 123))]) == 10_000_000_123
assert capture.kernel_timestamp_ns([]) is None
print('PASS bounded IPv6 DTLS metadata parsing and kernel timestamp validation')
