# Внешние компоненты

## bt-proxy

Автор/проект: [Marker284/yandex-station-extended](https://github.com/Marker284/yandex-station-extended).
Закреплённый commit: `2eda824ae9aa357d37fbb0877b62f85c0abf19c7`.

- [Исходники и Cargo.lock](https://github.com/Marker284/yandex-station-extended/tree/2eda824ae9aa357d37fbb0877b62f85c0abf19c7/contrib/bt-proxy)
- [Бинарник](https://raw.githubusercontent.com/Marker284/yandex-station-extended/2eda824ae9aa357d37fbb0877b62f85c0abf19c7/contrib/bin/bt-proxy)
- [bus.conf](https://raw.githubusercontent.com/Marker284/yandex-station-extended/2eda824ae9aa357d37fbb0877b62f85c0abf19c7/contrib/bt-proxy/bus.conf)

Это неизменённый upstream ARMv7/musl-помощник, не C-приёмник YaDLNA. Его SHA-256 закреплён в `vendor/bt-proxy/manifest.json`. Наш init-скрипт адаптирован для проверенной Mini 2.

При получении не обнаружен отдельный LICENSE в корне upstream. Мы не выдаём ему лицензию MIT и не утверждаем наличие разрешения на дальнейшее распространение. Поэтому бинарник и конфиг доступны локально в исключённом из Git `local-deps/`, а готовый установочный tar — в исключённом `dist/`. Отслеживаемый Git содержит только ссылки, проверяемый загрузчик и контрольные суммы. Перед самостоятельной публикацией стороннего компонента проверьте условия у upstream. Для нового клона `python scripts/fetch-bt-proxy.py` самостоятельно получает ровно закреплённые файлы.

## libcurl 8.15.0

Статически включена в бинарник приёмника. [Исходник](https://curl.se/download/curl-8.15.0.tar.xz), [лицензия](third_party/CURL-LICENSE.txt). Сборка описана в build/Dockerfile.https. Наша MIT-лицензия не заменяет лицензию curl.

## Штатные библиотеки

OpenSSL 1.1, GStreamer/GLib, ALSA, Expat, D-Bus и libc берутся из прошивки и в дистрибутив не входят. Их системные файлы установщик не заменяет. Копии прошивки для стенда не публикуются.

## Источники руководства

- [Rootfs, SSH и автозапуск](https://hackalice.me/start/ssh/#dropbear): использован механизм init.d и временного remount rootfs; установка Dropbear, отключение OTA и изменение ADB-маркеров не входят в YaDLNA.
- [Mini 2](https://github.com/Marker284/yandex-station-extended/blob/main/docs/devices/station-mini2.md)
- [bt-proxy](https://github.com/Marker284/yandex-station-extended/blob/main/docs/recipes/bt-proxy.md)
- [GStreamer](https://gstreamer.freedesktop.org/documentation/), [libcurl](https://curl.se/libcurl/c/)

MIT в LICENSE применяется только к новому коду и документации YaDLNA, если конкретный файл не отмечен иначе.
