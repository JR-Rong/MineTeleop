#!/usr/bin/env python3
"""Bounded Linux bench capture: DTLS and optional RTP timing, never payloads.

Requires CAP_NET_RAW. Run only for the exact isolated test endpoints and keep
its output private. Kernel timestamps use CLOCK_REALTIME; calibration records
permit local comparison with monotonic application timestamps. Packet records
cannot identify individual encrypted SCTP messages.
"""
import argparse
import ipaddress
import json
import os
import pwd
import signal
import socket
import struct
import time
from pathlib import Path

ETH_P_ALL = 0x0003
SO_TIMESTAMPNS = getattr(socket, 'SO_TIMESTAMPNS', 35)
SOL_PACKET = getattr(socket, 'SOL_PACKET', 263)
PACKET_STATISTICS = 6


def classify(packet, local, peer, include_rtp=False):
    """Parse one cooked IPv6 packet; reject fragmentation, truncation and unknown peers."""
    if len(packet) < 48 or packet[0] >> 4 != 6:
        return None
    source, dest = packet[8:24], packet[24:40]
    if source == local and dest == peer:
        direction = 'out'
    elif source == peer and dest == local:
        direction = 'in'
    else:
        return None
    next_header, pos = packet[6], 40
    for _ in range(8):
        if next_header == 17:
            break
        if next_header not in (0, 43, 60) or len(packet) < pos + 2:
            return None
        next_header, length = packet[pos], (packet[pos + 1] + 1) * 8
        pos += length
    else:
        return None
    if next_header != 17 or len(packet) < pos + 8:
        return None
    src_port, dst_port, udp_length = struct.unpack_from('!HHH', packet, pos)
    payload = pos + 8
    meta = {'direction': direction, 'src_port': src_port, 'dst_port': dst_port,
            'udp_bytes': udp_length}
    if (udp_length >= 21 and len(packet) >= payload + 13 and
            packet[payload] in (20, 21, 22, 23, 24, 25) and
            packet[payload + 1:payload + 3] in (b'\xfe\xff', b'\xfe\xfd', b'\xfe\xfc')):
        meta.update(dtls_type=packet[payload],
                    dtls_epoch=int.from_bytes(packet[payload + 3:payload + 5], 'big'),
                    dtls_record_seq=int.from_bytes(packet[payload + 5:payload + 11], 'big'))
    elif (include_rtp and udp_length >= 16 and len(packet) >= payload + 8 and
          packet[payload] >> 6 == 2 and 192 <= packet[payload + 1] <= 223):
        meta.update(media_type='rtcp', rtcp_type=packet[payload + 1])
    elif (include_rtp and udp_length >= 20 and len(packet) >= payload + 12 and
          packet[payload] >> 6 == 2 and
          udp_length >= 20 + 4 * (packet[payload] & 15) and
          len(packet) >= payload + 12 + 4 * (packet[payload] & 15)):
        meta.update(media_type='rtp',
                    rtp_seq=int.from_bytes(packet[payload + 2:payload + 4], 'big'),
                    rtp_timestamp=int.from_bytes(packet[payload + 4:payload + 8], 'big'),
                    rtp_ssrc=int.from_bytes(packet[payload + 8:payload + 12], 'big'))
    else:
        return None
    if include_rtp:
        meta['dscp'] = ((packet[0] & 15) << 2) | (packet[1] >> 6)
    return meta


def kernel_timestamp_ns(ancillary):
    for level, kind, data in ancillary:
        if level == socket.SOL_SOCKET and kind == SO_TIMESTAMPNS and len(data) >= struct.calcsize('@ll'):
            sec, ns = struct.unpack_from('@ll', data)
            if sec > 0 and 0 <= ns < 1_000_000_000:
                return sec * 1_000_000_000 + ns
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--interface', required=True)
    ap.add_argument('--local', required=True)
    ap.add_argument('--peer', required=True)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--stop-file', required=True, type=Path)
    ap.add_argument('--seconds', type=int, default=2400)
    ap.add_argument('--owner', default='nvidia')
    ap.add_argument('--include-rtp', action='store_true', help='Also record RTP/RTCP header identities, never media data')
    args = ap.parse_args()
    local, peer = ipaddress.IPv6Address(args.local), ipaddress.IPv6Address(args.peer)
    if not local.is_global or not peer.is_global or local == peer or not 1 <= args.seconds <= 3600:
        ap.error('distinct global IPv6 endpoints and duration of 1–3600 seconds required')
    if args.output.exists() or args.stop_file.exists():
        ap.error('output and stop marker must not already exist')
    owner = pwd.getpwnam(args.owner)
    # Linux's outgoing packet tap requires ETH_P_ALL. Filtering by ETH_P_IPV6
    # here loses egress evidence; classify() still rejects all other traffic.
    sock = socket.socket(socket.AF_PACKET, socket.SOCK_DGRAM, socket.htons(ETH_P_ALL))
    sock.setsockopt(socket.SOL_SOCKET, SO_TIMESTAMPNS, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind((args.interface, 0))
    sock.settimeout(.25)
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    os.fchown(fd, owner.pw_uid, owner.pw_gid)
    stopped = False
    def request_stop(*_):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    counts = {'dtls': 0, 'rtp': 0, 'rtcp': 0}
    missing_timestamp = truncated = 0
    started, next_calibration = time.monotonic(), 0
    with os.fdopen(fd, 'w', buffering=1) as out:
        def emit(obj):
            out.write(json.dumps(obj, separators=(',', ':')) + '\n')
        emit({'event': 'start', 'interface': args.interface, 'duration_limit_s': args.seconds,
              'payloads_recorded': False, 'include_rtp': args.include_rtp})
        print('DTLS_METADATA_CAPTURE_READY', flush=True)
        while not stopped and time.monotonic() - started < args.seconds and not args.stop_file.exists():
            if time.monotonic() >= next_calibration:
                mono_before = time.monotonic_ns(); wall = time.time_ns(); mono_after = time.monotonic_ns()
                emit({'event': 'clock', 'realtime_ns': wall, 'monotonic_before_ns': mono_before,
                      'monotonic_after_ns': mono_after})
                next_calibration = time.monotonic() + 5
            try:
                packet, ancillary, flags, _ = sock.recvmsg(256, socket.CMSG_SPACE(struct.calcsize('@ll')))
            except socket.timeout:
                continue
            observed = time.monotonic_ns()
            meta = classify(packet, local.packed, peer.packed, args.include_rtp)
            if meta is None:
                continue
            stamp = kernel_timestamp_ns(ancillary)
            if stamp is None:
                missing_timestamp += 1
            if flags & socket.MSG_TRUNC:
                truncated += 1
            kind = meta.pop('media_type', 'dtls')
            meta.update({'event': kind, 'kernel_realtime_ns': stamp, 'observed_monotonic_ns': observed})
            emit(meta)
            counts[kind] += 1
        try:
            packets, drops = struct.unpack('II', sock.getsockopt(SOL_PACKET, PACKET_STATISTICS, 8))
        except OSError:
            packets = drops = None
        emit({'event': 'summary', 'duration_s': round(time.monotonic() - started, 3),
              'dtls_records': counts['dtls'], 'rtp_records': counts['rtp'], 'rtcp_records': counts['rtcp'],
              'missing_kernel_timestamps': missing_timestamp, 'payload_truncated_by_design': truncated,
              'packet_socket_packets': packets, 'packet_socket_drops': drops})
    sock.close()
    print('DTLS_METADATA_CAPTURE_STOPPED', flush=True)


if __name__ == '__main__':
    main()
