#!/usr/bin/env python3
"""Build a deterministic local installation tar, verifying release/dependency hashes."""
from pathlib import Path
import hashlib, io, json, tarfile
root = Path(__file__).resolve().parents[1]
release = json.loads((root / 'releases/manifest.json').read_text())
deps = json.loads((root / 'vendor/bt-proxy/manifest.json').read_text())
def verified(path, info):
    data = path.read_bytes()
    if len(data) != info['size'] or hashlib.sha256(data).hexdigest() != info['sha256']:
        raise SystemExit(str(path) + ': checksum mismatch; abort')
    return data
items = {'bin/station-dlna': verified(root/'releases'/release['file'], release)}
for name, info in deps['files'].items():
    items['bt-proxy/' + name] = verified(root/'local-deps/bt-proxy'/name, info)
for name in ['S55zbtproxy', 'S99zdlna']:
    items['init/' + name] = (root/'packaging'/name).read_bytes()
items['install-on-station.sh'] = (root/'scripts/install-on-station.sh').read_bytes()
items['LICENSE'] = (root/'LICENSE').read_bytes()
items['third_party/CURL-LICENSE.txt'] = (root/'third_party/CURL-LICENSE.txt').read_bytes()
items['THIRD_PARTY.md'] = (root/'THIRD_PARTY.md').read_bytes()
for name in ['INSTALL-ADB.md', 'UPDATE-ROLLBACK.md', 'TROUBLESHOOTING.md', 'TEST-REPORT.md', 'ARCHITECTURE.md']:
    items['docs/' + name] = (root/'docs'/name).read_bytes()
for name, data in items.items():
    if name.startswith('init/') or name.endswith('.sh'):
        if b'\r' in data: raise SystemExit(name + ': requires LF line endings')
# Never include this manifest in its own hash list.
items['SHA256SUMS'] = ''.join(hashlib.sha256(v).hexdigest()+'  '+k+'\n' for k,v in sorted(items.items())).encode()
out = root/'dist'; out.mkdir(exist_ok=True)
archive = out/'yadlna-0.1.1-mini2.tar'
with tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as tar:
    for name, data in sorted(items.items()):
        ti = tarfile.TarInfo(name); ti.size=len(data); ti.mtime=0
        ti.uid=ti.gid=0; ti.uname=ti.gname='root'
        ti.mode=0o755 if name in ['bin/station-dlna','bt-proxy/bt-proxy','install-on-station.sh'] or name.startswith('init/') else 0o644
        tar.addfile(ti, io.BytesIO(data))
digest=hashlib.sha256(archive.read_bytes()).hexdigest()
(archive.with_suffix('.tar.sha256')).write_text(digest+'  '+archive.name+'\n',encoding='ascii',newline='\n')
print(str(archive)); print('SHA256 '+digest)
