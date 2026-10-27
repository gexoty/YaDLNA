# Обновление и откат

Нужен root ADB. Выполнять блоки поочерёдно с проверкой результата. При неожиданной ошибке остановиться. Для уже работающей версии 0.1.1 повторная установка не нужна.

## Обновить только приёмник

Пример для существующей установки с неизменным ABI и init-скриптами. На ПК:

```powershell
adb push releases/station-dlna-0.1.1-armhf /data/opt/station-dlna/bin/station-dlna.new
adb shell
```

Внутри ADB shell:

```sh
set -eu
cd /data/opt/station-dlna/bin
sha256sum station-dlna.new
```

Сверить SHA-256 с `releases/manifest.json`. Для 0.1.1: `17bc84eaf317b8dedca9db763e8c629e8215a1914b510a61d6915f8e1b427fca`. Далее:

```sh
export LD_LIBRARY_PATH=/system/vendor/quasar/libs:/system/vendor/quasar:/lib:/usr/lib
export GST_PLUGIN_PATH=/system/vendor/quasar/libs
export GST_REGISTRY=/tmp/station-dlna-gst-registry.bin
export GST_REGISTRY_FORK=no
chmod 755 station-dlna.new
./station-dlna.new --check
test ! -e station-dlna.before-update
cp -p station-dlna station-dlna.before-update
/etc/init.d/S99zdlna stop
sleep 3
ps
```

Убедиться, что старый процесс именно station-dlna завершился. Если он остался, остановиться и диагностировать. Затем:

```sh
mv station-dlna.new station-dlna
/etc/init.d/S99zdlna start
sleep 3
cat /run/station-dlna/receiver.log
```

Rootfs remount и перезапуск Bluetooth для этого не нужны. Откат бинарника: остановить S99zdlna, проверить завершение процесса, скопировать `station-dlna.before-update` поверх `station-dlna`, вернуть права 755 и запустить S99zdlna. Не использовать killall для штатных служб.

## Полностью отменить новую установку

Сначала проверить, что два init-файла принадлежат YaDLNA. После частичной установки некоторые файлы могут отсутствовать: сверить состояние, прежде чем выполнять действия. Потребуется перезагрузка для возврата штатной Bluetooth-топологии.

В ADB shell:

```sh
set -eu
if [ -x /etc/init.d/S99zdlna ]; then /etc/init.d/S99zdlna stop; fi
sleep 3
if [ -x /etc/init.d/S55zbtproxy ]; then /etc/init.d/S55zbtproxy stop; fi
sleep 3
ps
```

Проверить завершение собственных station-dlna и bt-proxy. Не завершать все процессы dbus. Посмотреть activation-файлы:

```sh
B=/usr/share/dbus-1/system-services/org.bluez.service
for f in "$B" "$B.disabled" /data/opt/station-dlna/backup/org.bluez.service; do
    if [ -e "$f" ]; then ls -l "$f"; else echo "Absent: $f"; fi
done
```

Для полностью завершённой установки ожидается: основной activation отсутствует, `.disabled` существует и совпадает с backup. Проверить:

```sh
test ! -e "$B"
test -f "$B.disabled"
cmp "$B.disabled" /data/opt/station-dlna/backup/org.bluez.service
mount -o remount,rw /
```

Только при успешной проверке восстановить activation и убрать собственные init-файлы:

```sh
mv "$B.disabled" "$B"
rm /etc/init.d/S55zbtproxy /etc/init.d/S99zdlna
sync
mount -o remount,ro /
reboot
```

Если действие после remount rw завершилось ошибкой, прежде всего выполнить `sync` и `mount -o remount,ro /`, затем разобраться с причиной. Если `.disabled` отсутствует, восстановление из backup возможно только после проверки отсутствия другого штатного activation-файла. Не перезаписывать неизвестную конфигурацию.

После загрузки проверить штатный Bluetooth и ответы Алисы. Каталоги `/data/opt/station-dlna` и `/data/opt/btproxy` можно оставить как backup: без init-файлов они не запускаются. Не удалять целиком `/data/opt`, где могут быть другие программы.
