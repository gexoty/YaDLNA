#!/usr/bin/env python3
"""Loopback UPnP/MPRIS tests; never connect to the station."""
import concurrent.futures
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import queue
import signal
import shlex
import socket
import subprocess
import threading
import time
import unittest
import xml.etree.ElementTree as ET
from xml.sax.saxutils import escape

ROOT=Path(__file__).resolve().parents[1]
BIN=str(ROOT/'build/out/station-dlna-test')
AVT='urn:schemas-upnp-org:service:AVTransport:1'
RCS='urn:schemas-upnp-org:service:RenderingControl:1'
CMS='urn:schemas-upnp-org:service:ConnectionManager:1'
MP='org.mpris.MediaPlayer2.Player'
PORT=49495
events=queue.Queue()

def http(method,path,body=None,headers=None):
    c=HTTPConnection('127.0.0.1',PORT,timeout=3)
    c.request(method,path,body,headers or {})
    r=c.getresponse();out=(r.status,dict(r.getheaders()),r.read());c.close();return out

def soap(service,action,**args):
    name={AVT:'avtransport',RCS:'renderingcontrol',CMS:'connectionmanager'}[service]
    if service!=CMS:args={'InstanceID':'0',**args}
    payload=''.join('<{0}>{1}</{0}>'.format(k,escape(str(v))) for k,v in args.items())
    body='<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><u:{0} xmlns:u="{1}">{2}</u:{0}></s:Body></s:Envelope>'.format(action,service,payload)
    return http('POST','/control/'+name,body,{'SOAPACTION':'"'+service+'#'+action+'"','Content-Type':'text/xml'})

def fields(reply):
    return {e.tag.rsplit('}',1)[-1]:e.text or '' for e in ET.fromstring(reply[2]).iter()}

def mpris(method,*args,interface=MP):
    return subprocess.check_output(['dbus-send','--system','--print-reply','--dest=org.mpris.MediaPlayer2.StationDLNA','/org/mpris/MediaPlayer2',interface+'.'+method]+list(args),env=ENV,stderr=subprocess.STDOUT).decode()

class Callback(BaseHTTPRequestHandler):
    def do_NOTIFY(self):
        body=self.rfile.read(int(self.headers['Content-Length']))
        events.put((dict(self.headers),body));self.send_response(200);self.end_headers()
    def log_message(self,*args):pass

class Tests(unittest.TestCase):
    def setUp(self):
        soap(AVT,'Stop')
        soap(AVT,'SetAVTransportURI',CurrentURI='',CurrentURIMetaData='')
        soap(RCS,'SetMute',Channel='Master',DesiredMute=0)
    def track(self,n='one'):
        return soap(AVT,'SetAVTransportURI',CurrentURI='http://127.0.0.1/music/'+n+'.mp3',CurrentURIMetaData='<DIDL-Lite xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Title &amp; '+n+'</dc:title></DIDL-Lite>')
    def test_description_and_scpd(self):
        r=http('GET','/description.xml');self.assertEqual(r[0],200)
        tree=ET.fromstring(r[2]);self.assertIn('MediaRenderer:1',r[2].decode())
        ns={'d':'urn:schemas-upnp-org:device-1-0','s':'urn:schemas-upnp-org:service-1-0'}
        self.assertEqual(len(tree.findall('.//d:service',ns)),3)
        for name in ['avtransport','renderingcontrol','connectionmanager']:
            r=http('GET','/scpd/'+name+'.xml');self.assertEqual(r[0],200);t=ET.fromstring(r[2]);variables={v.find('s:name',ns).text for v in t.findall('.//s:stateVariable',ns)}
            for arg in t.findall('.//s:relatedStateVariable',ns):self.assertIn(arg.text,variables)
    def test_transport_pause_resume_and_stop(self):
        self.assertEqual(self.track()[0],200);self.assertEqual(soap(AVT,'Play',Speed=1)[0],200)
        self.assertEqual(fields(soap(AVT,'GetTransportInfo'))['CurrentTransportState'],'PLAYING')
        self.assertEqual(soap(AVT,'Pause')[0],200);p=fields(soap(AVT,'GetPositionInfo'))['RelTime'];time.sleep(.08)
        self.assertEqual(fields(soap(AVT,'GetPositionInfo'))['RelTime'],p)
        self.assertEqual(soap(AVT,'Play',Speed=1)[0],200);soap(AVT,'Stop');self.assertEqual(fields(soap(AVT,'GetTransportInfo'))['CurrentTransportState'],'STOPPED')
    def test_mpris_commands_and_metadata(self):
        self.track();self.assertIn('Title & one',mpris('Get','string:'+MP,'string:Metadata',interface='org.freedesktop.DBus.Properties'))
        mpris('Play');self.assertEqual(fields(soap(AVT,'GetTransportInfo'))['CurrentTransportState'],'PLAYING')
        mpris('Pause');self.assertEqual(fields(soap(AVT,'GetTransportInfo'))['CurrentTransportState'],'PAUSED_PLAYBACK')
        mpris('PlayPause');self.assertEqual(fields(soap(AVT,'GetTransportInfo'))['CurrentTransportState'],'PLAYING')
        mpris('Stop');self.assertIn('Stopped',mpris('GetAll','string:'+MP,interface='org.freedesktop.DBus.Properties'))
    def test_next_previous(self):
        self.track();soap(AVT,'SetNextAVTransportURI',NextURI='http://127.0.0.1/two.mp3',NextURIMetaData='');soap(AVT,'Play',Speed=1)
        mpris('Next');self.assertTrue(fields(soap(AVT,'GetMediaInfo'))['CurrentURI'].endswith('/two.mp3'))
        mpris('Previous');self.assertTrue(fields(soap(AVT,'GetMediaInfo'))['CurrentURI'].endswith('/one.mp3'))
    def test_next_preserves_pause(self):
        self.track();soap(AVT,'Play',Speed=1);soap(AVT,'Pause');soap(AVT,'SetNextAVTransportURI',NextURI='http://127.0.0.1/two.mp3',NextURIMetaData='');mpris('Next')
        self.assertEqual(fields(soap(AVT,'GetTransportInfo'))['CurrentTransportState'],'PAUSED_PLAYBACK')
    def test_uri_schemes_and_injection(self):
        for uri in ['file:///etc/passwd','ftp://localhost/foo','http://','http:///etc/passwd','http://x/\nfoo']:
            self.assertEqual(soap(AVT,'SetAVTransportURI',CurrentURI=uri,CurrentURIMetaData='')[0],500)
        self.assertEqual(soap(AVT,'SetAVTransportURI',CurrentURI='http://localhost/a"!filesrc',CurrentURIMetaData='')[0],200)
    def test_volume_and_mute(self):
        self.assertEqual(soap(RCS,'SetVolume',Channel='Master',DesiredVolume=37)[0],200)
        self.assertEqual(fields(soap(RCS,'GetVolume',Channel='Master'))['CurrentVolume'],'37')
        mpris('Set','string:'+MP,'string:Volume','variant:double:0.42',interface='org.freedesktop.DBus.Properties')
        self.assertEqual(fields(soap(RCS,'GetVolume',Channel='Master'))['CurrentVolume'],'42')
        soap(RCS,'SetMute',Channel='Master',DesiredMute='true');self.assertEqual(fields(soap(RCS,'GetMute',Channel='Master'))['CurrentMute'],'1')
        self.assertEqual(soap(RCS,'SetVolume',Channel='Master',DesiredVolume=101)[0],500)
    def test_seek_and_queries(self):
        self.track();soap(AVT,'Play',Speed=1);self.assertEqual(soap(AVT,'Seek',Unit='REL_TIME',Target='0:01:20')[0],200)
        self.assertEqual(fields(soap(AVT,'GetPositionInfo'))['RelTime'],'0:01:20')
        self.assertEqual(soap(AVT,'Seek',Unit='TRACK_NR',Target='1')[0],500)
        for action in ['GetTransportSettings','GetDeviceCapabilities','GetCurrentTransportActions']:self.assertEqual(soap(AVT,action)[0],200)
        for action in ['GetProtocolInfo','GetCurrentConnectionIDs']:self.assertEqual(soap(CMS,action)[0],200)
        self.assertEqual(soap(CMS,'GetCurrentConnectionInfo',ConnectionID=0)[0],200)
    def test_soap_faults(self):
        self.assertEqual(fields(soap(AVT,'Play',Speed=1))['errorCode'],'702')
        self.assertEqual(fields(soap(AVT,'Bogus'))['errorCode'],'401')
        self.assertEqual(fields(soap(AVT,'GetTransportInfo',InstanceID=1))['errorCode'],'718')
        self.track();self.assertEqual(fields(soap(AVT,'Play',Speed=2))['errorCode'],'717')
    def test_dtd_and_xml_depth(self):
        for body in ['<!DOCTYPE a [<!ENTITY e SYSTEM "file:///etc/passwd">]><a>&e;</a>','<x>'*30+'text'+'</x>'*30,'not XML']:
            r=http('POST','/control/avtransport',body,{'SOAPACTION':'"'+AVT+'#Play"'});self.assertEqual(r[0],500)
        self.assertEqual(http('GET','/description.xml')[0],200)
    def test_http_bounds_and_smuggling(self):
        for req in [b'POST /control/avtransport HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 10\r\n\r\n',b'POST /control/avtransport HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n',b'POST /control/avtransport HTTP/1.1\r\nContent-Length: 9999999\r\n\r\n']:
            with socket.create_connection(('127.0.0.1',PORT),3) as s:s.sendall(req);self.assertIn(b'400',s.recv(8192).split(b'\r\n')[0])
    def test_fragmented_http(self):
        with socket.create_connection(('127.0.0.1',PORT),3) as s:
            for p in [b'GET /description',b'.xml HTTP/1.1\r\nHost: local\r\n',b'\r\n']:s.sendall(p);time.sleep(.02)
            self.assertIn(b'200 OK',s.recv(8192))
    def test_event_subscription_and_ssrf(self):
        while not events.empty():events.get_nowait()
        r=http('SUBSCRIBE','/event/avtransport',headers={'NT':'upnp:event','CALLBACK':'<http://127.0.0.1:%d/cb>'%CBPORT,'TIMEOUT':'Second-120'})
        self.assertEqual(r[0],200);sid=r[1]['SID'];h,b=events.get(timeout=3);self.assertEqual(h['SEQ'],'0');ET.fromstring(b)
        self.track();h,b=events.get(timeout=3);self.assertIn(b'LastChange',b)
        self.assertEqual(http('SUBSCRIBE','/event/avtransport',headers={'SID':sid,'TIMEOUT':'Second-300'})[0],200)
        self.assertEqual(http('UNSUBSCRIBE','/event/avtransport',headers={'SID':sid})[0],200)
        self.assertEqual(http('SUBSCRIBE','/event/avtransport',headers={'NT':'upnp:event','CALLBACK':'<http://192.0.2.1:80/>'})[0],412)
    def test_ssdp_discovery(self):
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
            s.bind(('127.0.0.1',0));s.settimeout(3)
            s.sendto(b'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 1\r\nST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n',('127.0.0.1',1900))
            data,_=s.recvfrom(4096);self.assertIn(b'200 OK',data);self.assertIn(b'/description.xml',data)
    def test_parallel_control(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as e:
            results=list(e.map(lambda _:soap(AVT,'GetTransportInfo')[0],range(40)))
        self.assertEqual(results,[200]*40)

if __name__=='__main__':
    bus=subprocess.check_output(['dbus-daemon','--session','--fork','--print-address=1','--print-pid=1']).decode().splitlines()
    ENV={**os.environ,'DBUS_SYSTEM_BUS_ADDRESS':bus[0]}
    cb=ThreadingHTTPServer(('127.0.0.1',0),Callback);CBPORT=cb.server_port
    threading.Thread(target=cb.serve_forever,daemon=True).start()
    log=open(ROOT/'build/out/test-daemon.log','w')
    command=shlex.split(os.environ.get('STATION_TEST_COMMAND',BIN))
    proc=subprocess.Popen(command+['--interface','lo','--port',str(PORT),'--mock-audio'],env=ENV,stdout=log,stderr=log)
    try:
        for _ in range(100):
            try:http('GET','/description.xml');break
            except OSError:
                if proc.poll() is not None:
                    raise RuntimeError('test daemon failed to start; inspect build/out/test-daemon.log')
                time.sleep(.05)
        result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests))
        assert proc.poll() is None, 'daemon crashed'
        raise SystemExit(0 if result.wasSuccessful() else 1)
    finally:
        proc.terminate();proc.wait(timeout=5);os.kill(int(bus[1]),signal.SIGTERM);cb.shutdown();log.close()
