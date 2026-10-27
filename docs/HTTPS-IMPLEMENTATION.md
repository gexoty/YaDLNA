# Station DLNA 0.1.1 — исправление Navidrome HTTPS

Новый C-код включает статическую libcurl 8.15.0 (только HTTP/HTTPS), которая использует уже установленные OpenSSL 1.1 и CA bundle станции. Системные библиотеки не входят в комплект и не заменяются. Небольшой локальный потоковый адаптер между libcurl и GStreamer работает на 127.0.0.1 в одном рабочем потоке; не сохраняет музыку на диск и не загружает её целиком. Сохраняются декодирование, pause/resume и HTTP Range seek штатным GStreamer. Параметры авторизации источника не передаются в GStreamer и не выводятся в обычный журнал.

TLS: проверяются цепочка сертификатов и имя хоста. HTTPS→HTTP downgrade при редиректе запрещён. CA bundle по умолчанию /system/vendor/quasar/ca-certificates.crt. Для стенда можно задать STATION_CA_BUNDLE.

Устранено преждевременное Playing: сначала UPnP TRANSITIONING; MPRIS Playing публикуется после сообщения pipeline о переходе в PLAYING. Ошибки записываются с числовыми кодами без полного URL.

## Сборка на ПК с Docker

```sh
docker build -t station-dlna-builder .
docker build -t station-dlna-validator -f build/Dockerfile.validation .
docker build -t station-dlna-https-builder -f build/Dockerfile.https .
docker run --rm -v "$PWD:/src" station-dlna-https-builder sh build/build-https.sh
```

Результат: build/out/station-dlna-0.1.1. Путь содержит только новый приёмник; bt-proxy и init-скрипты остаются прежними. Для PowerShell используйте `-v "${PWD}:/src"`. При необходимости явно выберите ваш Docker builder, имеющий доступ к локальным базовым образам.

В тестах протокола используется --mock-audio; для эмуляции настоящего декодирования нужны самостоятельно прочитанные с устройства библиотеки и QEMU. Копии прошивки и адреса с токенами здесь не включены. Переменные STATION_GST_LIBRARY/STATION_AUDIO_SINK предназначены для стенда, штатный init их не задаёт.

Пройдены 15 тестов протокола/MPRIS на x86-64 и ARM; HTTP WAV с pause/resume/seek; исходная HTTPS-ссылка Navidrome на ARM с библиотеками этой станции; отрицательная проверка недоверенного сертификата (curl 60), при которой Playing не публикуется. На физической Mini 2 проверены продвижение позиции, рабочий PCM и отсутствие ошибок pipeline; пользователь подтвердил слышимую музыку.

Статическая libcurl распространяется согласно third_party/CURL-LICENSE.txt. Новый код — MIT. Штатные OpenSSL/GStreamer/ALSA/GLib имеют собственные условия и в архив не включены. Источники API: https://curl.se/libcurl/c/ и https://gstreamer.freedesktop.org/documentation/.
