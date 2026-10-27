#!/bin/sh
# Fresh Mini 2 install only. Run manually from /data/opt/station-dlna.
# Does not obtain root, modify firmware libraries, or reboot automatically.
set -eu
P=/data/opt/station-dlna
B=/usr/share/dbus-1/system-services/org.bluez.service
ROOT_RW=0
finish() {
    rc=$?
    trap - EXIT
    if [ "$ROOT_RW" = 1 ]; then
        sync
        mount -o remount,ro / || {
            echo 'ERROR: remount ro failed; inspect mount immediately' >&2
            rc=1
        }
    fi
    if [ "$rc" -ne 0 ]; then
        echo 'STOP: installation failed. Do not reboot or continue blindly; inspect completed commands and docs/UPDATE-ROLLBACK.md.' >&2
    fi
    exit "$rc"
}
trap finish EXIT
[ "$(id -u)" = 0 ] || { echo 'Root ADB shell required' >&2; exit 1; }
model=$(tr -d '\000' < /proc/device-tree/model)
case "$model" in
    'Yandex Station Mini 2'*) ;;
    *) echo "Unsupported model: $model; only Mini 2 was tested" >&2; exit 1 ;;
esac
for command in sha256sum awk usleep pidof dbus-daemon chmod cp mv mount sync; do
    command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 1; }
done
cd "$P"
[ ! -e /etc/init.d/S55zbtproxy ] && [ ! -e /etc/init.d/S99zdlna ] || { echo 'Existing install detected; use update guide' >&2; exit 1; }
[ ! -e /data/opt/btproxy ] || { echo 'Existing btproxy directory: stop and inspect' >&2; exit 1; }
[ -f "$B" ] && [ ! -e "$B.disabled" ] || { echo 'Unexpected BlueZ activation state' >&2; exit 1; }
[ ! -e "$P/backup/org.bluez.service" ] || { echo 'Existing backup: stop and inspect' >&2; exit 1; }
for s in S51bluetoothd S52bluealsa S53bluealsa-aplay S54default-bluez-agent; do
    [ -x "/etc/init.d/$s" ] || { echo "Missing stock init: $s" >&2; exit 1; }
done
for f in /usr/bin/dbus-daemon /system/vendor/quasar/libs/libquasar_gstreamer.so /system/vendor/quasar/ca-certificates.crt; do
    [ -r "$f" ] || { echo "Missing runtime dependency: $f" >&2; exit 1; }
done
sha256sum -c SHA256SUMS
chmod 755 "$P/bin/station-dlna"
export LD_LIBRARY_PATH=/system/vendor/quasar/libs:/system/vendor/quasar:/lib:/usr/lib
export GST_PLUGIN_PATH=/system/vendor/quasar/libs
export GST_REGISTRY=/tmp/station-dlna-gst-registry.bin
export GST_REGISTRY_FORK=no
"$P/bin/station-dlna" --check
# All compatibility checks above complete before the persistent activation change.
set -x
mkdir -p "$P/backup" /data/opt/btproxy /data/opt/home
cp "$B" "$P/backup/org.bluez.service"
cp "$P/bt-proxy/bt-proxy" /data/opt/btproxy/bt-proxy
cp "$P/bt-proxy/bus.conf" /data/opt/btproxy/bus.conf
chmod 755 /data/opt/btproxy/bt-proxy
chmod 644 /data/opt/btproxy/bus.conf
mount -o remount,rw /
ROOT_RW=1
cp "$P/init/S55zbtproxy" /etc/init.d/S55zbtproxy
cp "$P/init/S99zdlna" /etc/init.d/S99zdlna
chmod 755 /etc/init.d/S55zbtproxy /etc/init.d/S99zdlna
mv "$B" "$B.disabled"
sync
mount -o remount,ro /
ROOT_RW=0
set +x
echo 'Installed. Rootfs remounted ro. No reboot performed. Pull backup to PC, then reboot manually.'
