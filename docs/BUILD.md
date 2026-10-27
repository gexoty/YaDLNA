# Сборка и упаковка на ПК

Для установки готового релиза Docker не нужен. Нужны ADB и Python 3 на ПК. Python на станции не используется.

## Проверка готового комплекта

Из корня репозитория, PowerShell:

```powershell
python scripts/fetch-bt-proxy.py
python scripts/package.py
python scripts/verify-release.py
```

Загрузчик проверяет размер и SHA-256 внешних файлов по закреплённому commit. Если локальные файлы уже правильные, скачивания нет. Упаковщик создаёт `dist/yadlna-0.1.1-mini2.tar`, соседний SHA-256 и внутренний манифест. Тот же набор входных файлов даёт тот же tar. Архив содержит Unix-права и LF. Собственная контрольная сумма манифеста не включается в него самого.

`local-deps/` и `dist/` исключены из Git из-за стороннего bt-proxy с неуточнёнными условиями распространения. На этом ПК они уже подготовлены; после клонирования создаются командами выше. Подробнее в [THIRD_PARTY.md](../THIRD_PARTY.md).

## Пересборка приёмника

Нужны Docker Desktop с Linux containers и доступ к архивным Debian-репозиториям и curl.se.

```powershell
docker build -t station-dlna-builder .
docker build -t station-dlna-validator -f build/Dockerfile.validation .
docker build -t station-dlna-https-builder -f build/Dockerfile.https .
docker run --rm -v "${PWD}:/src" station-dlna-https-builder sh build/build-https.sh
```

Результат: `build/out/station-dlna-0.1.1`, сведения о линковке и host-бинарник. Сборка запускает 15 тестов UPnP/MPRIS с mock audio. Они не подключаются к колонке и не проверяют реальную слышимость.

Дополнительная проверка AddressSanitizer/UBSan:

```powershell
docker run --rm -v "${PWD}:/src" station-dlna-https-builder sh build/test-sanitized.sh
```

Для проверки ARM-протокола без копии прошивки можно использовать ARM-библиотеки контейнера:

```powershell
docker run --rm -v "${PWD}:/src" -e "STATION_TEST_COMMAND=qemu-arm -L /usr/arm-linux-gnueabihf -E LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf /src/build/out/station-dlna-0.1.1" station-dlna-https-builder python3 tests/integration.py
```

Для синтаксиса init и установщика:

```powershell
docker run --rm -v "${PWD}:/src:ro" station-dlna-https-builder sh -c 'sh -n scripts/install-on-station.sh && sh -n packaging/S55zbtproxy && sh -n packaging/S99zdlna'
```

Debian buster выбран для совместимости с glibc станции. Это архивная сборочная среда. Версия базового образа и curl закреплены, но APT-пакеты не образуют полностью герметичную побайтово воспроизводимую сборку. SHA-256 в `releases/manifest.json` относится к сохранённому артефакту, проверенному на станции.

После изменений сначала проверить новую сборку. Не заменять сохранённый релиз автоматически: осознанно обновить версию, релизный файл, manifest, упаковщик и документацию. Несовпадающий hash упаковщик отклонит.

## Стенд настоящего декодирования

`tests/audio_emulated.py` проверяет HTTP WAV, seek, pause/resume/stop и EOS/Next с ARM GStreamer прошивки и fakesink. `tests/https_negative.py` проверяет отказ недоверенному TLS-сертификату. `tests/proxy_emulated.py` проверяет настоящий ARM bt-proxy с двумя изолированными D-Bus-шинами; Quasar и голос не эмулируются.

Для `sh build/emulate.sh` внутри контейнера нужны приватные монтирования:

- `/src` — репозиторий после сборки;
- `/runtime.tar` — ARM sysroot с `lib/`, `usr/lib/`, ELF-загрузчиком и зависимостями, включая OpenSSL 1.1;
- `/gcc.so` — совместимый ARM `libgcc_s.so.1`;
- `/vendor` — `libquasar_gstreamer.so`, `libglib.so`, `libiconv.so`, их зависимости и `ca-certificates.crt`, полученные владельцем со своей станции;
- `/upstream/bt-proxy` — локальный внешний помощник.

Это дополнительный разработческий стенд. Прошивка и готовый sysroot в Git не входят. Реальный HTTPS-поток Navidrome проверялся отдельно; использованный приватный URL с авторизацией не публикуется.
