#!/bin/sh
set -eu
cd /src
mkdir -p /tmp/rt
tar xf /runtime.tar -C /tmp/rt
cp /gcc.so /tmp/rt/usr/lib/libgcc_s.so.1
export STATION_TEST_COMMAND='qemu-arm -L /tmp/rt -E LD_LIBRARY_PATH=/tmp/rt/lib:/tmp/rt/usr/lib:/vendor -E STATION_GST_LIBRARY=/vendor/libquasar_gstreamer.so /src/build/out/station-dlna-0.1.1'
export STATION_CA_BUNDLE=/vendor/ca-certificates.crt
python3 tests/integration.py
python3 tests/audio_emulated.py
python3 tests/https_negative.py
python3 tests/proxy_emulated.py
