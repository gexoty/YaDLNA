#!/usr/bin/env python3
"""Fetch pinned external artifacts locally. Does not connect to the station."""
from pathlib import Path
import hashlib, json, urllib.request
root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / 'vendor/bt-proxy/manifest.json').read_text())
out = root / 'local-deps/bt-proxy'
out.mkdir(parents=True, exist_ok=True)
for name, info in manifest['files'].items():
    target = out / name
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == info['sha256']:
        print(name + ': already verified')
        continue
    data = urllib.request.urlopen(info['url'], timeout=60).read()
    if len(data) != info['size'] or hashlib.sha256(data).hexdigest() != info['sha256']:
        raise SystemExit(name + ': checksum/size mismatch; nothing installed')
    target.write_bytes(data)
    print(name + ': downloaded and verified')
