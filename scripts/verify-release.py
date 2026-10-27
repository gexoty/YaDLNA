#!/usr/bin/env python3
"""Offline verification: release, local dependency hashes, tar members and LF."""
from pathlib import Path, PurePosixPath
import hashlib, json, tarfile
root=Path(__file__).resolve().parents[1]
for manifest, directory in [('releases/manifest.json','releases'),('vendor/bt-proxy/manifest.json','local-deps/bt-proxy')]:
    obj=json.loads((root/manifest).read_text())
    entries=obj.get('files', {obj.get('file',''):obj})
    for name,info in entries.items():
        p=root/directory/name
        data=p.read_bytes()
        assert len(data)==info['size'] and hashlib.sha256(data).hexdigest()==info['sha256'], p
archive=root/'dist/yadlna-0.1.1-mini2.tar'
assert hashlib.sha256(archive.read_bytes()).hexdigest()==archive.with_suffix('.tar.sha256').read_text().split()[0]
with tarfile.open(archive) as tf:
    members=tf.getmembers(); names=[m.name for m in members]
    assert len(names)==len(set(names))
    for m in members:
        assert m.isfile() and not PurePosixPath(m.name).is_absolute() and '..' not in PurePosixPath(m.name).parts
    sums=tf.extractfile('SHA256SUMS').read().decode().splitlines()
    checked=[]
    for line in sums:
        digest,name=line.split('  ',1); data=tf.extractfile(name).read()
        assert hashlib.sha256(data).hexdigest()==digest,name
        if name.startswith('init/') or name.endswith('.sh'): assert b'\r' not in data,name
        checked.append(name)
    assert set(checked)==set(names)-{'SHA256SUMS'}
    assert tf.getmember('bin/station-dlna').mode==0o755
print('PASS: release/dependencies, archive hash, safe members, full manifest, executable mode, LF')
