#!/usr/bin/env python3
"""Real firmware GStreamer via ARM emulation; decoded PCM goes to fakesink.
No hardware access and no connection to the physical station.
"""
import http.server
import io
import os
from pathlib import Path
import shlex
import struct
import subprocess
import threading
import time
import wave
import integration as it

raw=io.BytesIO()
with wave.open(raw,'wb') as w:
    w.setparams((1,2,16000,0,'NONE','not compressed'))
    w.writeframes(struct.pack('<h',1000)*16000*12)
wav=raw.getvalue()

class Media(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        content=wav
        if self.path.startswith('/short'):
            b=io.BytesIO()
            with wave.open(b,'wb') as w:
                w.setparams((1,2,16000,0,'NONE','not compressed'));w.writeframes(struct.pack('<h',1000)*16000)
            content=b.getvalue()
        start=0
        if self.headers.get('Range','').startswith('bytes='):
            start=int(self.headers['Range'][6:].split('-')[0])
        self.send_response(206 if start else 200);self.send_header('Content-Type','audio/x-wav');self.send_header('Accept-Ranges','bytes')
        if start:self.send_header('Content-Range','bytes %d-%d/%d'%(start,len(content)-1,len(content)))
        self.send_header('Content-Length',str(len(content)-start));self.end_headers()
        try:self.wfile.write(content[start:])
        except (BrokenPipeError,ConnectionResetError):pass
    def log_message(self,*a):pass

server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Media)
threading.Thread(target=server.serve_forever,daemon=True).start()
it.PORT=49496
logfile=it.ROOT/'build/out/audio-emulated.log'
env={**os.environ,'STATION_AUDIO_SINK':'fakesink sync=true','STATION_DEBUG_AUDIO':'1','GST_REGISTRY':'/tmp/emulated-gst-registry','GST_REGISTRY_FORK':'no'}
cmd=shlex.split(os.environ['STATION_TEST_COMMAND'])
with logfile.open('w') as log:
    p=subprocess.Popen(cmd+['--interface','lo','--port',str(it.PORT),'--mock-mixer','--no-dbus'],env=env,stderr=log,stdout=log)
    try:
        for _ in range(100):
            try:it.http('GET','/description.xml');break
            except OSError:
                if p.poll() is not None:raise RuntimeError(logfile.read_text())
                time.sleep(.1)
        url='http://127.0.0.1:%d/test.wav'%server.server_port
        assert it.soap(it.AVT,'SetAVTransportURI',CurrentURI=url,CurrentURIMetaData='')[0]==200
        assert it.soap(it.AVT,'Play',Speed=1)[0]==200
        time.sleep(1.5)
        f=it.fields(it.soap(it.AVT,'GetTransportInfo'));assert f['CurrentTransportState']=='PLAYING',(f,logfile.read_text())
        pos=it.fields(it.soap(it.AVT,'GetPositionInfo'));assert pos['TrackDuration']=='0:00:12',pos
        assert pos['RelTime']!='0:00:00',pos
        assert it.soap(it.AVT,'Pause')[0]==200
        paused=it.fields(it.soap(it.AVT,'GetPositionInfo'))['RelTime'];time.sleep(1.2)
        assert it.fields(it.soap(it.AVT,'GetPositionInfo'))['RelTime']==paused
        assert it.soap(it.AVT,'Play',Speed=1)[0]==200
        time.sleep(.5)
        assert it.fields(it.soap(it.AVT,'GetTransportInfo'))['CurrentTransportState']=='PLAYING'
        assert it.soap(it.AVT,'Seek',Unit='REL_TIME',Target='0:00:06')[0]==200
        time.sleep(.3)
        assert it.fields(it.soap(it.AVT,'GetPositionInfo'))['RelTime']>='0:00:06'
        it.soap(it.AVT,'Stop')
        short='http://127.0.0.1:%d/short.wav'%server.server_port
        it.soap(it.AVT,'SetAVTransportURI',CurrentURI=short,CurrentURIMetaData='')
        it.soap(it.AVT,'SetNextAVTransportURI',NextURI=short+'?next',NextURIMetaData='')
        it.soap(it.AVT,'Play',Speed=1);time.sleep(2.8)
        assert it.fields(it.soap(it.AVT,'GetMediaInfo'))['CurrentURI']==short+'?next'
        assert it.fields(it.soap(it.AVT,'GetTransportInfo'))['CurrentTransportState']=='STOPPED'
        assert p.poll() is None
        print('PASS: ARM binary + copied firmware libs: HTTP WAV decode, duration, position, real pause/resume/stop/seek, EOS advances queued URI then stops; PCM -> fakesink; hardware mixer mocked')
    finally:
        p.terminate();p.wait(timeout=10);server.shutdown()
