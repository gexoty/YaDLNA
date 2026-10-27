#!/bin/sh
set -eu
cd /src
python3 build/generate.py
mkdir -p build/out
gcc -std=c11 -O1 -g -no-pie -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I/opt/curl-host/include $(pkg-config --cflags dbus-1 gstreamer-1.0) src/station_dlna.c \
    -o build/out/station-dlna-asan /opt/curl-host/lib/libcurl.a -lssl -lcrypto -ldbus-1 -lexpat -lasound -ldl -lm -pthread
STATION_TEST_COMMAND=/src/build/out/station-dlna-asan \
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 python3 tests/integration.py
