#!/usr/bin/env python3
"""Pinned coturn/SQLite integration; confined to loopback and temporary state."""
import importlib.util
import json
import os
from types import SimpleNamespace
import shutil
import sqlite3
import subprocess
import tempfile
import time
from pathlib import Path

spec=importlib.util.spec_from_file_location('relay_manager',Path(__file__).parents[1]/'relay'/'manager.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def wait(predicate,seconds=12):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if predicate():return
        time.sleep(.15)
    raise AssertionError('coturn transition timed out')

def main():
    version=subprocess.check_output(['turnserver','--version'],stderr=subprocess.STDOUT,text=True).strip()
    assert version=='4.6.1',version
    with tempfile.TemporaryDirectory(prefix='mine-relay-test-') as root:
        root=Path(root);db=root/'turn.sqlite';shutil.copyfile('/var/lib/turn/turndb',db)
        config=root/'turn.conf';config.write_text(f'listening-ip=127.0.0.1\nrelay-ip=127.0.0.1\nlistening-port=13478\nrealm=mine.test\nuserdb={db}\nuse-auth-secret\nfingerprint\nno-tls\nno-dtls\ncli-ip=127.0.0.1\ncli-port=15766\ncli-password=integration-only-password\ncli-max-output-sessions=10000\nno-multicast-peers\nmin-port=53000\nmax-port=53020\nlog-file=stdout\n')
        with open(root/'turn.log','w') as log:
            proc=subprocess.Popen(['turnserver','-c',str(config)],stdout=log,stderr=subprocess.STDOUT)
            try:
                cli=m.Coturn('integration-only-password',15766)
                def ready():
                    try:cli.sessions();return True
                    except (OSError,RuntimeError):return False
                wait(ready)
                identity='a'*32;secret='lease-specific-test-secret'
                with sqlite3.connect(db) as connection:connection.execute('INSERT OR IGNORE INTO turn_secret(realm,value) VALUES(?,?)',('mine.test',secret))
                wait(lambda:not m.allocate_probe('127.0.0.1',13478,'mine.test',secret,identity))
                wait(lambda:len(cli.sessions(':'+identity+':'))>=1)
                for actor in ('vehicle-1','driver-1','driver-1'):
                    assert not m.allocate_probe('127.0.0.1',13478,'mine.test',secret,identity,actor)
                wait(lambda:len(cli.sessions(':'+identity+':'))>=4)
                sessions=cli.sessions(':'+identity+':');assert {s['username'].split(':')[-1] for s in sessions}=={'probe','vehicle-1','driver-1'}
                for s in sessions:cli.cancel(s['id'])
                wait(lambda:not cli.sessions(':'+identity+':'))
                # Credential remains valid until the realm secret is removed.
                assert not m.allocate_probe('127.0.0.1',13478,'mine.test',secret,identity)
                with sqlite3.connect(db) as connection:connection.execute('DELETE FROM turn_secret WHERE realm=? AND value=?',('mine.test',secret))
                wait(lambda:m.allocate_probe('127.0.0.1',13478,'mine.test',secret,identity))
                for s in cli.sessions(':'+identity+':'):cli.cancel(s['id'])
                wait(lambda:not cli.sessions(':'+identity+':'))
                # Completion must remain empty for five seconds, not just one ps.
                for _ in range(6):
                    assert not cli.sessions(':'+identity+':')
                    assert m.allocate_probe('127.0.0.1',13478,'mine.test',secret,identity)
                    time.sleep(1)
                if os.environ.get('TEST_RELAY_INTERFACE'):
                    password=root/'cli-password';password.write_text('integration-only-password')
                    args=SimpleNamespace(state_dir=str(root),database=str(db),realm='mine.test',cli_password_file=str(password),cli_port=15766,turn_port=13478,interface=os.environ['TEST_RELAY_INTERFACE'])
                    lease={'lease_id':identity,'secret':secret,'state':'confirmed','expires_at_ms':int(time.time()*1000)+60000,'egress_bps':5500000}
                    m.atomic_json(root/'ledger.json',{'instance':'fixture','leases':{identity:lease}})
                    manager=m.Manager(args)
                    def activated():
                        manager.tick();return json.loads((root/'status.json').read_text())['leases'][identity]['activated']
                    wait(activated)
                    lease['state']='active';m.atomic_json(root/'ledger.json',{'instance':'fixture','leases':{identity:lease}})
                    for actor in ('vehicle-1','driver-1','driver-1'):
                        assert not m.allocate_probe('127.0.0.1',13478,'mine.test',secret,identity,actor)
                    manager.tick();assert json.loads((root/'status.json').read_text())['leases'][identity]['allocations']>=3
                    lease['state']='revoking';m.atomic_json(root/'ledger.json',{'instance':'fixture','leases':{identity:lease}})
                    def reclaimed():
                        manager.tick();row=json.loads((root/'status.json').read_text())['leases'][identity]
                        return row['secret_deleted'] and row['credential_rejected'] and row['allocations']==0 and row.get('empty_for_ms',0)>=5000
                    wait(reclaimed)
                    manager.db.close();restarted=m.Manager(args);restarted.tick()
                    assert json.loads((root/'status.json').read_text())['leases'][identity]['empty_for_ms']<5000
                    restarted.db.close()
                    print(json.dumps({'manager':'actual coturn + shaped leaf counters','activation':True,'reclamation':'confirmed empty five seconds','restart':'restarts empty interval','result':'passed'}))
                print(json.dumps({'coturn':'4.6.1','backend':'SQLite','allocation_cancel':'asynchronous-confirmed','removed_secret':'old_credential_rejected','empty_seconds':5,'result':'passed'}))
            finally:
                proc.terminate()
                try:proc.wait(5)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
                if proc.returncode not in (0,-15):print((root/'turn.log').read_text()[-2000:])
if __name__=='__main__':main()
