#!/usr/bin/env python3
"""Exercise the actual ARM bt-proxy bridge with two isolated local D-Bus buses.
The BlueZ caller replaces Quasar in this test; no hardware/voice recognition.
"""
import os
import shlex
import signal
import subprocess
import time
import integration as it

def bus():return subprocess.check_output(['dbus-daemon','--session','--fork','--print-address=1','--print-pid=1']).decode().splitlines()
public=bus();private=bus();it.PORT=49497
receiver_env={**os.environ,'DBUS_SYSTEM_BUS_ADDRESS':private[0]}
proxy_env={**os.environ,'DBUS_SYSTEM_BUS_ADDRESS':public[0],'BT_PRIVATE_BUS':private[0],'BT_PROXY_ALIASES':'StationDLNA=DLNA'}
name='org.mpris.MediaPlayer2.StationDLNA';h=0x811c9dc5
for b in name.encode():h=((h^b)*0x01000193)&0xffffffff
mac='02_A1_'+'_'.join('%02X'%((h>>shift)&255) for shift in (24,16,8,0))
path='/org/bluez/hci0/dev_'+mac
def bluez(member,*args,at=None,iface='org.bluez.MediaPlayer1'):
    return subprocess.check_output(['dbus-send','--system','--print-reply','--reply-timeout=5000','--dest=org.bluez',at or path+'/player0',iface+'.'+member]+list(args),env=proxy_env,stderr=subprocess.STDOUT).decode()

log=(it.ROOT/'build/out/proxy-emulated.log').open('w')
p=q=None
try:
    p=subprocess.Popen(shlex.split(os.environ['STATION_TEST_COMMAND'])+['--interface','lo','--port',str(it.PORT),'--mock-audio'],env=receiver_env,stdout=log,stderr=log)
    q=subprocess.Popen(['qemu-arm','/upstream/bt-proxy'],env=proxy_env,stdout=log,stderr=log)
    for _ in range(100):
        try:it.http('GET','/description.xml');break
        except OSError:time.sleep(.1)
    # Let the proxy install its match rules before publishing Playing.
    time.sleep(1)
    it.soap(it.AVT,'SetAVTransportURI',CurrentURI='http://127.0.0.1/one.mp3',CurrentURIMetaData='')
    it.soap(it.AVT,'SetNextAVTransportURI',NextURI='http://127.0.0.1/two.mp3',NextURIMetaData='')
    it.soap(it.AVT,'Play',Speed=1)
    time.sleep(3)
    assert q.poll() is None,'proxy exited'
    bluez('Pause');assert it.fields(it.soap(it.AVT,'GetTransportInfo'))['CurrentTransportState']=='PAUSED_PLAYBACK'
    bluez('Play');assert it.fields(it.soap(it.AVT,'GetTransportInfo'))['CurrentTransportState']=='PLAYING'
    bluez('Next');assert it.fields(it.soap(it.AVT,'GetMediaInfo'))['CurrentURI'].endswith('two.mp3')
    bluez('Previous');assert it.fields(it.soap(it.AVT,'GetMediaInfo'))['CurrentURI'].endswith('one.mp3')
    bluez('Set','string:org.bluez.MediaTransport1','string:Volume','variant:uint16:64',at=path+'/fd0',iface='org.freedesktop.DBus.Properties')
    assert abs(int(it.fields(it.soap(it.RCS,'GetVolume',Channel='Master'))['CurrentVolume'])-50)<=1
    print('PASS: real ARM bt-proxy -> ARM renderer MPRIS: BlueZ Pause, Play, Next, Previous, Volume. Quasar/voice/hardware not emulated.')
finally:
    for proc in [q,p]:
        if proc:
            proc.terminate()
            try:proc.wait(timeout=10)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
    for b in [public,private]:os.kill(int(b[1]),signal.SIGTERM)
    log.close()
