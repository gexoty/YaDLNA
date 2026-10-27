#!/bin/sh
set -eu
cd /src
python3 build/generate.py
mkdir -p build/out
CFLAGS='-std=c11 -Os -flto -ffunction-sections -fdata-sections -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wall -Wextra -Wformat=2 -Wno-misleading-indentation'
export PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
arm-linux-gnueabihf-gcc $CFLAGS -I/opt/curl-arm/include $(pkg-config --cflags dbus-1 gstreamer-1.0) src/station_dlna.c -o build/out/station-dlna-0.1.1 -Wl,--gc-sections,-z,relro,-z,now -pie -fPIE /opt/curl-arm/lib/libcurl.a -lssl -lcrypto -ldbus-1 -lexpat -lasound -ldl -lm -pthread
arm-linux-gnueabihf-strip build/out/station-dlna-0.1.1
arm-linux-gnueabihf-readelf -d build/out/station-dlna-0.1.1 > build/out/linkage-0.1.1.txt
export PKG_CONFIG_LIBDIR=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig
gcc $CFLAGS -I/opt/curl-host/include $(pkg-config --cflags dbus-1 gstreamer-1.0) src/station_dlna.c -o build/out/station-dlna-test /opt/curl-host/lib/libcurl.a -lssl -lcrypto -ldbus-1 -lexpat -lasound -ldl -lm -pthread
python3 tests/integration.py
