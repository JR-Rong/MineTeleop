"""Real TLS/WSS, local ephemeral port, no application reads after upgrade."""
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
                if not stop.wait(12):raise RuntimeError('client did not terminate no-read fixture')
        except BaseException as error:errors.append(str(error))
    worker=threading.Thread(target=serve);worker.start()
    try:
        result=subprocess.run([sys.argv[1],'--tls-client',f'wss://127.0.0.1:{listener.getsockname()[1]}/',str(cert)],
            capture_output=True,text=True,timeout=15)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
        evidence=json.loads(result.stdout);evidence['fixture']='real TLS and WSS upgrade; peer stops reading'
        print(json.dumps(evidence,sort_keys=True))
    finally:
        stop.set();listener.close();worker.join(timeout=12)
    if worker.is_alive() or errors:raise RuntimeError(str(errors or 'TLS fixture did not join'))
