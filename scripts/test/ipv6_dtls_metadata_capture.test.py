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

def datagram(payload, source=peer, destination=local):
    udp = struct.pack('!HHHH', 1234, 5678, len(payload) + 8, 0) + payload
    # DSCP46, ECN0; only the metadata parser sees the synthetic payload.
    header = bytes([0x6b, 0x80, 0, 0]) + struct.pack('!HBB', len(udp), 17, 64) + source + destination
    return header + udp

rtp = struct.pack('!BBHII', 0x80, 96, 65535, 123456, 42) + b'private-video-data'
packet = datagram(rtp)
assert capture.classify(packet, local, peer) is None  # Media observation stays opt-in.
x = capture.classify(packet, local, peer, include_rtp=True)
assert x == {'direction': 'in', 'src_port': 1234, 'dst_port': 5678,
             'udp_bytes': len(rtp) + 8, 'media_type': 'rtp', 'rtp_seq': 65535,
             'rtp_timestamp': 123456, 'rtp_ssrc': 42, 'dscp': 46}
assert capture.classify(datagram(rtp, local, peer), local, peer, True)['direction'] == 'out'
assert capture.classify(packet[:59], local, peer, True) is None
assert capture.classify(datagram(bytes([0x8f]) + rtp[1:]), local, peer, True) is None  # Missing CSRCs.
assert capture.classify(packet, local, bytes(16), True) is None
assert capture.classify(bytes([0x45]) + packet[1:], local, peer, True) is None
rtcp = struct.pack('!BBHI', 0x80, 201, 1, 42)
assert capture.classify(datagram(rtcp), local, peer, True)['media_type'] == 'rtcp'
assert capture.classify(datagram(rtcp), local, peer) is None
assert capture.classify(p, local, peer, True)['dtls_record_seq'] == 123
assert 'private-video-data' not in str(x)
print('PASS bounded IPv6 DTLS/optional RTP metadata, endpoint isolation and kernel timestamps')
