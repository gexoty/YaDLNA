import os,sys,subprocess,ssl,http.server,threading,time,shlex
from pathlib import Path
sys.path.insert(0,'/src/tests');import integration as it
it.PORT=49499
subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout','/tmp/untrusted.key','-out','/tmp/untrusted.crt','-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):self.send_response(200);self.end_headers();self.wfile.write(b'not audio')
    def log_message(self,*a):pass
s=http.server.ThreadingHTTPServer(('127.0.0.1',0),H);ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain('/tmp/untrusted.crt','/tmp/untrusted.key');s.socket=ctx.wrap_socket(s.socket,server_side=True);threading.Thread(target=s.serve_forever,daemon=True).start()
env={**os.environ,'STATION_CA_BUNDLE':'/vendor/ca-certificates.crt','STATION_AUDIO_SINK':'fakesink sync=true','GST_REGISTRY_FORK':'no','GST_REGISTRY':'/tmp/untrusted-registry'}
log=Path('/src/build/out/untrusted-certificate.log')
with log.open('w') as f:
    p=subprocess.Popen(shlex.split(os.environ['STATION_TEST_COMMAND'])+['--mock-mixer','--no-dbus','--interface','lo','--port',str(it.PORT)],env=env,stdout=f,stderr=f)
    try:
        for _ in range(100):
            try:it.http('GET','/description.xml');break
            except OSError:time.sleep(.1)
        it.soap(it.AVT,'SetAVTransportURI',CurrentURI='https://localhost:%d/test.mp3'%s.server_port,CurrentURIMetaData='');it.soap(it.AVT,'Play',Speed=1)
        seen=[]
        for _ in range(80):
            state=it.fields(it.soap(it.AVT,'GetTransportInfo'));seen.append(state['CurrentTransportState'])
            if state['CurrentTransportStatus']=='ERROR_OCCURRED':break
            time.sleep(.05)
        assert 'PLAYING' not in seen,seen
        assert state['CurrentTransportState']=='STOPPED' and state['CurrentTransportStatus']=='ERROR_OCCURRED',state
        time.sleep(.1);assert 'transport=60' in log.read_text(),log.read_text()
        print('PASS: untrusted TLS certificate rejected (curl 60); never advertised PLAYING; observed states:',','.join(dict.fromkeys(seen)))
    finally:p.terminate();p.wait(timeout=10);s.shutdown()
