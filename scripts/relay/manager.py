#!/usr/bin/env python3
"""Local coturn allocator/reaper. Only this process may write status.json.

Run with the restricted root service capability defined in systemd. Never expose its CLI or state directory
through the signaling API. A failed measurement/reclamation retains the budget.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import re
import socket
import sqlite3
import struct
import subprocess
import time
from pathlib import Path


def atomic_json(path, value):
    tmp = path.with_suffix('.tmp')
    with open(tmp, 'w', opener=lambda p, f: os.open(p, f, 0o600)) as out:
        json.dump(value, out, separators=(',', ':'))
        out.flush()
        os.fsync(out.fileno())
    os.replace(tmp, path)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


class Coturn:
    def __init__(self, password, port=5766):
        self.password, self.port = password, port

    def command(self, command):
        if '\n' in command or '\r' in command:
            raise ValueError('invalid management command')
        with socket.create_connection(('127.0.0.1', self.port), 2) as sock:
            sock.settimeout(2)
            def read_to(end):
                data = b''
                while not data.rstrip().endswith(end):
                    chunk = sock.recv(65536)
                    if not chunk or len(data) > 8 * 1024 * 1024:
                        raise RuntimeError('coturn CLI truncated')
                    data += chunk
                return data.decode('utf-8', 'replace')
            read_to(b'password:')
            sock.sendall(self.password.encode() + b'\r\n')
            read_to(b'>')
            sock.sendall(command.encode() + b'\r\n')
            return read_to(b'>')

    def sessions(self, pattern=''):
        text = self.command('psp ' + pattern if pattern else 'ps')
        if 'Warning: too many' in text:
            raise RuntimeError('allocation enumeration truncated')
        total = re.search(r'Total sessions[^\r\n]*: (\d+)', text)
        result = []
        for block in re.split(r'(?=\d+\) id=)', text):
            identity = re.search(r'id=(\d+), user <([^>]+)>', block)
            if not identity:
                continue
            usage = re.search(r'usage: rp=(\d+), rb=(\d+), sp=(\d+), sb=(\d+)', block)
            rate = re.search(r'rate: r=(\d+), s=(\d+), total=(\d+)', block)
            if not usage or not rate:
                raise RuntimeError('coturn usage fields missing')
            result.append({'id': identity[1], 'username': identity[2],
                           'sent_bytes': int(usage[4]), 'sent_bps': int(rate[2]) * 8})
        if not total or int(total[1]) != len(result):
            raise RuntimeError('allocation inventory incomplete')
        return result

    def cancel(self, identity):
        if not identity.isdecimal():
            raise ValueError('invalid session id')
        self.command('cs ' + identity)


def attribute(kind, data):
    return struct.pack('!HH', kind, len(data)) + data + bytes((-len(data)) % 4)


def allocate_probe(host, port, realm, secret, lease_id, actor="probe"):
    """A real UDP Allocate authentication probe; success is immediately released.

    None means transport/parser failure; it must never qualify reclamation.
    """
    tx = os.urandom(12)
    cookie = 0x2112A442
    def exchange(sock, attrs, key=None):
        body = b''.join(attrs)
        if key:
            header = struct.pack('!HHI12s', 3, len(body) + 24, cookie, tx)
            body += attribute(8, hmac.new(key, header + body, hashlib.sha1).digest())
        packet = struct.pack('!HHI12s', 3, len(body), cookie, tx) + body
        sock.sendto(packet, (host, port))
        answer, _ = sock.recvfrom(65535)
        if len(answer) < 20 or answer[8:20] != tx:
            raise RuntimeError('invalid TURN probe response')
        kind, length = struct.unpack_from('!HH', answer)
        fields = {}
        offset = 20
        while offset < 20 + length:
            tag, size = struct.unpack_from('!HH', answer, offset)
            fields[tag] = answer[offset+4:offset+4+size]
            offset += 4 + size + (-size) % 4
        return kind, fields
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(2)
        _, challenge = exchange(sock, [attribute(0x19, bytes([17, 0, 0, 0]))])
        if 0x15 not in challenge or 0x14 not in challenge:
            raise RuntimeError('TURN probe lacked nonce/realm')
        username = f'{int(time.time())+600}:{realm}:{lease_id}:{actor}'.encode()
        password = base64.b64encode(hmac.new(secret.encode(), username, hashlib.sha1).digest())
        key = hashlib.md5(username + b':' + challenge[0x14] + b':' + password).digest()
        kind, answer = exchange(sock, [attribute(6, username), attribute(0x14, challenge[0x14]),
                                      attribute(0x15, challenge[0x15]), attribute(0x19, bytes([17,0,0,0]))], key)
        if kind == 0x103:
            # Caller cancels the probe via the normal per-lease inventory.
            return False
        error = answer.get(9, b'')
        if kind != 0x113 or len(error) < 4:
            raise RuntimeError('unexpected TURN probe result')
        return error[2] * 100 + error[3] in (401, 441)


class Manager:
    def __init__(self, args):
        version=subprocess.check_output(['turnserver','--version'],stderr=subprocess.STDOUT,text=True).strip()
        if version!='4.6.1':raise RuntimeError('relay manager requires validated coturn 4.6.1 / SQLite')
        self.args = args
        self.root = Path(args.state_dir)
        self.cli = Coturn(Path(args.cli_password_file).read_text().strip(), args.cli_port)
        self.empty_since, self.previous, self.windows = {}, {}, {}
        self.egress_counter_invalid = False
        self.db = sqlite3.connect(args.database)
        # Match coturn's schema; do not create an incompatible lookalike table.
        if not self.db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='turn_secret'").fetchone():
            raise RuntimeError('coturn database has no turn_secret table')

    def secret(self, lease, active):
        with self.db:
            if active:
                self.db.execute('INSERT OR IGNORE INTO turn_secret(realm,value) VALUES(?,?)',
                                (self.args.realm, lease['secret']))
            else:
                self.db.execute('DELETE FROM turn_secret WHERE realm=? AND value=?',
                                (self.args.realm, lease.get('secret', '')))

    def tick(self):
        ledger = json.loads((self.root / 'ledger.json').read_text())
        now = int(time.time() * 1000)
        reports = {}
        all_sessions = self.cli.sessions()
        known_ids = set(ledger['leases'])
        known_secrets={lease['secret'] for lease in ledger['leases'].values() if 'secret' in lease}
        if any(row[0] not in known_secrets for row in self.db.execute('SELECT value FROM turn_secret WHERE realm=?',(self.args.realm,))):raise RuntimeError('unowned realm secret: finish maintenance migration')
        # A stale credential outside the ledger indicates incomplete migration.
        if any(not any(':'+key+':' in s['username'] for key in known_ids) for s in all_sessions):
            raise RuntimeError('unaccounted coturn allocation: finish maintenance migration')
        for identity, lease in ledger['leases'].items():
            pattern = ':' + identity + ':'
            sessions = [s for s in all_sessions if pattern in s['username']]
            state = lease['state']
            active = state in ('confirmed', 'active') and lease['expires_at_ms'] > now
            self.secret(lease, active)
            report = {'allocations': len(sessions), 'activated': False,
                      'secret_deleted': not active, 'credential_rejected': False}
            if active:
                # Database polling is asynchronous. Do not publish active before
                # coturn actually accepts this lease's newly installed secret.
                if state == 'confirmed':
                    rejected = allocate_probe('127.0.0.1', self.args.turn_port,
                                              self.args.realm, lease['secret'], identity)
                    report['activated'] = not rejected
                    for session in self.cli.sessions(pattern):
                        if session['username'].endswith(':probe'):
                            self.cli.cancel(session['id'])
                else:
                    report['activated'] = True
                self.empty_since.pop(identity, None)
            elif state != 'released':
                for session in sessions:
                    self.cli.cancel(session['id'])
                report['credential_rejected'] = allocate_probe('127.0.0.1', self.args.turn_port,
                                                               self.args.realm, lease['secret'], identity)
                remaining = self.cli.sessions(pattern)
                report['allocations'] = len(remaining)
                if not remaining and report['credential_rejected']:
                    self.empty_since.setdefault(identity, time.monotonic())
                    report['empty_for_ms'] = int((time.monotonic()-self.empty_since[identity])*1000)
                else:
                    self.empty_since.pop(identity, None)
            sent = sum(s['sent_bps'] for s in sessions)
            window = self.windows.setdefault(identity, [])
            window.append(sent)
            del window[:-5]
            report['allocation_egress_bps'] = sent
            report['window_egress_bps'] = sum(window)/len(window)
            report['over_budget'] = len(window)==5 and report['window_egress_bps'] > lease['egress_bps']
            reports[identity] = report
        # Count the actual shaped media class at the bottleneck, not whole-NIC
        # WSS traffic. Missing/ambiguous tc counters fail closed.
        if self.egress_counter_invalid:raise RuntimeError('media egress counter reset requires reconciliation restart')
        # iproute2 6.1 emits text for HTB classes even with -j. The dedicated
        # fq_codel leaf has JSON counters and exactly the same media parent.
        qdiscs = json.loads(subprocess.check_output(['tc', '-j', '-s', 'qdisc', 'show', 'dev', self.args.interface]))
        media = [row for row in qdiscs if row.get('parent')=='1:20' and row.get('kind')=='fq_codel']
        if len(media)!=1:
            raise RuntimeError('media egress shaping class 1:20 missing')
        counted = media[0].get('stats', media[0])
        bytes_sent = counted.get('bytes', counted.get('stats64', {}).get('bytes'))
        if bytes_sent is None:
            raise RuntimeError('media egress byte counter missing')
        prev = self.previous.get('egress')
        elapsed = time.monotonic()
        if prev is not None and bytes_sent<prev[0]:
            self.egress_counter_invalid=True
            raise RuntimeError('media egress counter reset; retain quota and stop new admission')
        egress = 0 if prev is None else max(0, (bytes_sent-prev[0])*8/(elapsed-prev[1]))
        self.previous['egress'] = (bytes_sent, elapsed)
        window=self.windows.setdefault('public-egress',[])
        window.append(egress);del window[:-5]
        active_ids=[key for key,lease in ledger['leases'].items() if lease['state'] in ('active','confirmed')]
        # With one active lease, all shaped public bytes belong to it. With
        # multiple leases, allocation counters provide the attribution weight.
        allocation_sum=sum(reports[key]['window_egress_bps'] for key in active_ids)
        for key in active_ids:
            weight=1 if len(active_ids)==1 else (reports[key]['window_egress_bps']/allocation_sum if allocation_sum else 1/len(active_ids))
            public_bps=sum(window)/len(window)*weight
            reports[key]['public_egress_bps']=public_bps
            reports[key]['over_budget']=reports[key]['over_budget'] or (len(window)==5 and public_bps>ledger['leases'][key]['egress_bps'])
        atomic_json(self.root/'status.json', {'instance': ledger['instance'], 'at_ms': now,
                    'healthy': True, 'media_egress_bps': egress, 'leases': reports})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--state-dir', required=True)
    parser.add_argument('--database', required=True)
    parser.add_argument('--realm', required=True)
    parser.add_argument('--cli-password-file', required=True)
    parser.add_argument('--cli-port', type=int, default=5766)
    parser.add_argument('--turn-port', type=int, default=3478)
    parser.add_argument('--interface', required=True)
    args = parser.parse_args()
    manager = Manager(args)
    while True:
        start = time.monotonic()
        try:
            manager.tick()
        except Exception as error:
            # Do not log secrets or credentials. Retain reservations on failure.
            print(json.dumps({'event':'relay_manager_failed', 'error':str(error)}), flush=True)
        time.sleep(max(0, 1-(time.monotonic()-start)))

if __name__ == '__main__':
    main()
