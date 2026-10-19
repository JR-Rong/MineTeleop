"""Real loopback TLS/WSS: deadlines, cancellation and notification wakeups."""
import base64,hashlib,json,os,pathlib,shutil,socket,ssl,subprocess,sys,tempfile,threading

openssl=shutil.which('openssl')
if not openssl and os.name=='nt':
    for candidate in [r'C:\Program Files\Git\usr\bin\openssl.exe',r'C:\Program Files\Git\mingw64\bin\openssl.exe']:
        if pathlib.Path(candidate).is_file():openssl=candidate;break
if not openssl:raise RuntimeError('openssl is required to create an ephemeral loopback TLS fixture')
with tempfile.TemporaryDirectory(prefix='mine-wss-send-') as tmp:
    root=pathlib.Path(tmp);cert=root/'ca.pem';key=root/'key.pem'
    subprocess.run([openssl,'req','-x509','-newkey','rsa:2048','-nodes','-days','1',
        '-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost,IP:127.0.0.1',
        '-keyout',str(key),'-out',str(cert)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(cert,key)
    def run_fixture(mode):
        listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4096)
        listener.bind(('127.0.0.1',0));listener.listen(1);listener.settimeout(10)
        stop=threading.Event();errors=[]
        def serve():
            try:
                raw,_=listener.accept();raw.settimeout(10)
                with context.wrap_socket(raw,server_side=True) as stream:
                    request=b''
                    while b'\r\n\r\n' not in request:request+=stream.recv(4096)
                    headers=dict(line.split(b':',1) for line in request.split(b'\r\n')[1:] if b':' in line)
                    nonce=next(value.strip() for name,value in headers.items() if name.lower()==b'sec-websocket-key')
                    accept=base64.b64encode(hashlib.sha1(nonce+b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest())
                    stream.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+b'\r\n\r\n')
                    if mode in ('--tls-client-drain','--tls-client-notification','--tls-client-readiness'):
                        def exact(size):
                            data=b''
                            while len(data)<size:
                                chunk=stream.recv(min(512,size-len(data)))
                                if not chunk:raise RuntimeError('incomplete TLS/WebSocket record')
                                data+=chunk
                            return data
                        def receive():
                            prefix=exact(2)
                            if prefix[0]!=0x81 or not prefix[1]&0x80:raise RuntimeError('unexpected WebSocket framing')
                            size=prefix[1]&0x7f
                            if size==126:size=int.from_bytes(exact(2),'big')
                            elif size==127:size=int.from_bytes(exact(8),'big')
                            if size>65536:raise RuntimeError('unbounded fixture record')
                            mask=exact(4);payload=exact(size)
                            return json.loads(bytes(byte^mask[offset%4] for offset,byte in enumerate(payload)))
                    if mode=='--tls-client-drain':
                        for index in range(8):
                            value=receive()
                            if value!={'probe':'x'*(16*1024),'seq':index}:raise RuntimeError('corrupted or replayed TLS record')
                        response=b'{"received":8}';stream.sendall(bytes([0x81,len(response)])+response)
                    elif mode=='--tls-client-notification':
                        for index,cut in enumerate((1,3,19)):
                            if receive()!={'split':index}:raise RuntimeError('unexpected split request')
                            payload=json.dumps({'split':index,'payload':'x'*256}).encode()
                            frame=b'\x81\x7e'+len(payload).to_bytes(2,'big')+payload
                            stream.sendall(frame[:cut])
                            if receive()!={'resume':index}:raise RuntimeError('outbound feedback was lost during partial receive')
                            stream.sendall(frame[cut:])
                    elif mode=='--tls-client-readiness':
                        if receive()!={'probe':'readiness'}:raise RuntimeError('unexpected readiness probe')
                        response=b'{"ready":true}';stream.sendall(bytes([0x81,len(response)])+response)
                    if not stop.wait(12):raise RuntimeError('client did not terminate no-read fixture')
            except BaseException as error:errors.append(str(error))
        worker=threading.Thread(target=serve);worker.start()
        try:
            result=subprocess.run([sys.argv[1],mode,f'wss://127.0.0.1:{listener.getsockname()[1]}/',str(cert)],
                capture_output=True,text=True,timeout=15)
            if result.returncode:raise RuntimeError(result.stdout+result.stderr)
            evidence=json.loads(result.stdout);evidence['fixture']='real loopback TLS and WSS upgrade';evidence['mode']=mode
            print(json.dumps(evidence,sort_keys=True))
        finally:
            stop.set();listener.close();worker.join(timeout=12)
        if worker.is_alive() or errors:raise RuntimeError(str(errors or 'TLS fixture did not join'))
    for mode in ['--tls-client-drain','--tls-client','--tls-client-cancel','--tls-client-notification','--tls-client-readiness']:run_fixture(mode)
