#!/usr/bin/env python3
"""Generate complete service descriptions from one small declarative table."""
import json
from pathlib import Path
from xml.etree.ElementTree import Element, SubElement, tostring

BASE = Path(__file__).resolve().parents[1]
SINK = ','.join('http-get:*:' + t + ':*' for t in (
    'audio/mpeg', 'audio/flac', 'audio/x-flac', 'audio/wav', 'audio/x-wav',
    'audio/ogg', 'application/ogg', 'audio/aac', 'audio/mp4', 'audio/x-m4a'))

def service(actions, variables):
    root = Element('scpd', xmlns='urn:schemas-upnp-org:service-1-0')
    spec = SubElement(root, 'specVersion')
    SubElement(spec, 'major').text = '1'
    SubElement(spec, 'minor').text = '0'
    al = SubElement(root, 'actionList')
    for name, args in actions.items():
        a = SubElement(al, 'action'); SubElement(a, 'name').text = name
        if args:
            lst = SubElement(a, 'argumentList')
            for arg, direction, var in args:
                p = SubElement(lst, 'argument')
                for k, v in [('name', arg), ('direction', direction), ('relatedStateVariable', var)]:
                    SubElement(p, k).text = v
    st = SubElement(root, 'serviceStateTable')
    for name, typ, event, allowed, rng in variables:
        s = SubElement(st, 'stateVariable', sendEvents='yes' if event else 'no')
        SubElement(s, 'name').text = name; SubElement(s, 'dataType').text = typ
        if allowed:
            a = SubElement(s, 'allowedValueList')
            for v in allowed: SubElement(a, 'allowedValue').text = v
        if rng:
            a = SubElement(s, 'allowedValueRange')
            for k,v in zip(('minimum','maximum','step'), rng): SubElement(a,k).text=str(v)
    return '<?xml version="1.0"?>' + tostring(root, encoding='unicode')

def v(name, typ='string', event=False, allowed=(), rng=None):
    return name, typ, event, allowed, rng

I = ('InstanceID', 'in', 'A_ARG_TYPE_InstanceID')
def ins(**kw): return [(k, 'in', val) for k,val in kw.items()]
def outs(**kw): return [(k, 'out', val) for k,val in kw.items()]
av = {
    'SetAVTransportURI': [I]+ins(CurrentURI='AVTransportURI',CurrentURIMetaData='AVTransportURIMetaData'),
    'SetNextAVTransportURI': [I]+ins(NextURI='NextAVTransportURI',NextURIMetaData='NextAVTransportURIMetaData'),
    'Play': [I]+ins(Speed='TransportPlaySpeed'), 'Pause':[I], 'Stop':[I], 'Next':[I], 'Previous':[I],
    'Seek':[I]+ins(Unit='A_ARG_TYPE_SeekMode',Target='A_ARG_TYPE_SeekTarget'),
    'SetPlayMode':[I]+ins(NewPlayMode='CurrentPlayMode'),
    'GetTransportInfo':[I]+outs(CurrentTransportState='TransportState',CurrentTransportStatus='TransportStatus',CurrentSpeed='TransportPlaySpeed'),
    'GetPositionInfo':[I]+outs(Track='CurrentTrack',TrackDuration='CurrentTrackDuration',TrackMetaData='CurrentTrackMetaData',TrackURI='CurrentTrackURI',RelTime='RelativeTimePosition',AbsTime='AbsoluteTimePosition',RelCount='RelativeCounterPosition',AbsCount='AbsoluteCounterPosition'),
    'GetMediaInfo':[I]+outs(NrTracks='NumberOfTracks',MediaDuration='CurrentMediaDuration',CurrentURI='AVTransportURI',CurrentURIMetaData='AVTransportURIMetaData',NextURI='NextAVTransportURI',NextURIMetaData='NextAVTransportURIMetaData',PlayMedium='PlaybackStorageMedium',RecordMedium='RecordStorageMedium',WriteStatus='RecordMediumWriteStatus'),
    'GetDeviceCapabilities':[I]+outs(PlayMedia='PossiblePlaybackStorageMedia',RecMedia='PossibleRecordStorageMedia',RecQualityModes='PossibleRecordQualityModes'),
    'GetTransportSettings':[I]+outs(PlayMode='CurrentPlayMode',RecQualityMode='CurrentRecordQualityMode'),
    'GetCurrentTransportActions':[I]+outs(Actions='CurrentTransportActions'),
}
av_vars = [v('LastChange',event=True), v('A_ARG_TYPE_InstanceID','ui4'),
           v('TransportState',allowed=('STOPPED','PLAYING','PAUSED_PLAYBACK','NO_MEDIA_PRESENT','TRANSITIONING')),
           v('TransportStatus',allowed=('OK','ERROR_OCCURRED')),v('TransportPlaySpeed',allowed=('1',)),
           v('CurrentPlayMode',allowed=('NORMAL',)),v('A_ARG_TYPE_SeekMode',allowed=('REL_TIME',)),
           v('A_ARG_TYPE_SeekTarget'),v('CurrentTrack','ui4'),v('NumberOfTracks','ui4'),
           v('RelativeCounterPosition','i4'),v('AbsoluteCounterPosition','i4')]
used = {var for args in av.values() for _,_,var in args}
for name in sorted(used-{x[0] for x in av_vars}): av_vars.append(v(name, 'uri' if name.endswith('URI') else 'string'))
CH=('Channel','in','A_ARG_TYPE_Channel')
rc={
    'ListPresets':[I]+outs(CurrentPresetNameList='PresetNameList'),
    'GetVolume':[I,CH]+outs(CurrentVolume='Volume'),
    'SetVolume':[I,CH]+ins(DesiredVolume='Volume'),
    'GetMute':[I,CH]+outs(CurrentMute='Mute'),
    'SetMute':[I,CH]+ins(DesiredMute='Mute'),
}
rc_vars=[v('LastChange',event=True),v('A_ARG_TYPE_InstanceID','ui4'),v('A_ARG_TYPE_Channel',allowed=('Master',)),v('Volume','ui2',rng=(0,100,1)),v('Mute','boolean'),v('PresetNameList')]
cm={
    'GetProtocolInfo':outs(Source='SourceProtocolInfo',Sink='SinkProtocolInfo'),
    'GetCurrentConnectionIDs':outs(ConnectionIDs='CurrentConnectionIDs'),
    'GetCurrentConnectionInfo':ins(ConnectionID='A_ARG_TYPE_ConnectionID')+outs(RcsID='A_ARG_TYPE_RcsID',AVTransportID='A_ARG_TYPE_AVTransportID',ProtocolInfo='A_ARG_TYPE_ProtocolInfo',PeerConnectionManager='A_ARG_TYPE_ConnectionManager',PeerConnectionID='A_ARG_TYPE_ConnectionID',Direction='A_ARG_TYPE_Direction',Status='A_ARG_TYPE_ConnectionStatus'),
}
cm_vars=[v('SourceProtocolInfo',event=True),v('SinkProtocolInfo',event=True),v('CurrentConnectionIDs',event=True),v('A_ARG_TYPE_ConnectionID','i4'),v('A_ARG_TYPE_RcsID','i4'),v('A_ARG_TYPE_AVTransportID','i4'),v('A_ARG_TYPE_ProtocolInfo'),v('A_ARG_TYPE_ConnectionManager'),v('A_ARG_TYPE_Direction',allowed=('Input',)),v('A_ARG_TYPE_ConnectionStatus',allowed=('OK',))]

services='';scpd=[]
for name,actions,variables in [('AVTransport',av,av_vars),('RenderingControl',rc,rc_vars),('ConnectionManager',cm,cm_vars)]:
    short=name.lower()
    services+=f'<service><serviceType>urn:schemas-upnp-org:service:{name}:1</serviceType><serviceId>urn:upnp-org:serviceId:{name}</serviceId><SCPDURL>/scpd/{short}.xml</SCPDURL><controlURL>/control/{short}</controlURL><eventSubURL>/event/{short}</eventSubURL></service>'
    scpd.append(service(actions,variables))

mpris='''<node><interface name="org.mpris.MediaPlayer2"><property name="Identity" type="s" access="read"/><property name="CanQuit" type="b" access="read"/><property name="CanRaise" type="b" access="read"/><property name="HasTrackList" type="b" access="read"/><property name="SupportedUriSchemes" type="as" access="read"/><property name="SupportedMimeTypes" type="as" access="read"/></interface><interface name="org.mpris.MediaPlayer2.Player">'''
for name in ['Play','Pause','PlayPause','Stop','Next','Previous']: mpris+=f'<method name="{name}"/>'
mpris+='<method name="Seek"><arg name="Offset" type="x" direction="in"/></method>'
for name,typ in [('PlaybackStatus','s'),('Metadata','a{sv}'),('Volume','d'),('Position','x'),('Rate','d'),('MinimumRate','d'),('MaximumRate','d'),('CanGoNext','b'),('CanGoPrevious','b'),('CanPlay','b'),('CanPause','b'),('CanSeek','b'),('CanControl','b')]:
    mpris+=f'<property name="{name}" type="{typ}" access="{"readwrite" if name=="Volume" else "read"}"/>'
mpris+='</interface><interface name="org.freedesktop.DBus.Properties"><method name="Get"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method><method name="GetAll"><arg type="s" direction="in"/><arg type="a{sv}" direction="out"/></method><method name="Set"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/></method><signal name="PropertiesChanged"><arg type="s"/><arg type="a{sv}"/><arg type="as"/></signal></interface></node>'
def cstr(s): return json.dumps(s,ensure_ascii=True)
(BASE/'src/descriptions.h').write_text('/* generated by build/generate.py */\n#define SINK_PROTOCOLS '+cstr(SINK)+'\n#define DEVICE_SERVICES '+cstr('<serviceList>'+services+'</serviceList>')+'\n#define MPRIS_XML '+cstr(mpris)+'\nstatic const char *SCPDS[] = {\n'+',\n'.join(cstr(s) for s in scpd)+'\n};\n')
