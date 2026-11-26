# j5xnlte-qmi-ril — Radio AIDL HAL (V1) поверх `/dev/wwan0qmi0`

Samsung Galaxy J5 2016 (j5xnlte, MSM8916, arm64), vendor API 34 (vendor от Android 14), система — GSI Android 15.
Раздел vendor на этом устройстве — `cache`.
English version: [README.md](README.md).

## Честный статус одной строкой

HAL **собран NDK в GitHub Actions и работает на настоящем телефоне** (GSI Android 15): регистрация в LTE, SIM в меню настроек, голосовые вызовы со звуком,
мут микрофона, SMS (входящие и исходящие), USSD и мобильный интернет проверены на устройстве (2026-11-26).
Это **не** «готовый серийный модем»: переключение «тихая/громкая» из интерфейса Android не сделано, штатный `rild` по-прежнему работает рядом с этим HAL.
Статус каждого пункта: **реализовано / собрано / проверено на устройстве / не проверено** — в таблице ниже.

## Что внутри

| Слой | Где | Что делает |
|---|---|---|
| QMUX/TLV-кодек | `qmi/src/qmi_codec.c` | кадры `0x01 | len | flags | svc | cid | SDU`, CTL (u8 txid) и сервисные (u16 txid) SDU, TLV, де-фреймер потока |
| Транспорт | `qmi/src/qmi_dev.c` | `/dev/wwan0qmi0`, поток чтения, таблица ожидающих транзакций (32), таймауты, повторы с новым txid, async-индикации, автопереподключение. QRTR не используется |
| Синхронный API модема | `ril/core/*.c` | CTL, DMS, UIM, NAS, WMS, VOICE (+USSD), WDS, WDA; очередь событий, рабочий поток |
| Radio HAL (**V1**) | `hal/src/*`, `hal/gen/*` | IRadioConfig, Modem, Sim, Network, Messaging, Voice, Data. Вызовы уходят в Executor (4 потока), binder-потоки не блокируются; USSD идёт в отдельном потоке |
| Аудио звонка | `hal/src/voice_audio.cc` | внутри HAL: порты CS-Voice, аналоговый тракт кодека MSM8916, открытие PCM, мут микрофона (из Audio HAL), громкость (из `STREAM_VOICE_CALL`); микшер через ioctl ALSA, без tinymix |
| Интеграция | `Android.bp`, `android/*` | cc_binary, init rc, VINTF, ueventd, telephony-features |
| Recovery-установщик | `android/recovery/`, `scripts/make-recovery-zip.sh` | zip для Lineage Recovery: установка в `/vendor` + автозапуск через init + отдельный zip отката |
| Установка adb | `scripts/install.sh`, `scripts/device/*` | отдельные шаги, откат до исходного состояния |
| Тесты | `tests/`, `scripts/run-all-tests.sh` | см. ниже |

### Какая версия AIDL
Vendor API 34 (FCM level 7): его матрица требует `android.hardware.radio.*` **V1**, поэтому сборка по умолчанию — V1 (`AIDL_VER=1`, VINTF `<version>1</version>`).
`AIDL_VER=2..4` собирает те же исходники против более новых замороженных снимков (на устройстве не проверялось).
В V1 нет `getImei` и `PhoneCapability.maxActiveVoice`: HAL выключает их при сборке (`hal/src/aidl_version.h`). Подробности — `docs/AUDIT_RU.md`.

### Что намеренно НЕ реализовано (отвечает `REQUEST_NOT_SUPPORTED`, а не «успехом»)
Запись через `iccIo`, `getSmscAddress`, `enableModem`, конференц-вызовы и переключение вызовов, CLIR (звонок с явным CLIR отклоняется),
логические каналы/STK, cell info, спутниковые методы V4, VoLTE/IMS, больше одного data call (второй → `MAX_ACTIVE_PDP_CONTEXT_REACHED`), IPv6,
экстренные вызовы без SIM.

## Таблица готовности

«Собрано» — собрано Android NDK в GitHub Actions (артефакт `j5xnlte-radio-hal-ndk`). Сборка в дереве AOSP (`Android.bp`, rc как часть образа) **не выполнялась**.
«На устройстве» — проверено на телефоне автором запроса.

| # | Критерий | Реализовано | Собрано (NDK) | Проверено на устройстве |
|---|---|---|---|---|
| 1 | Транспорт открывает `/dev/wwan0qmi0` | да | да | **да** |
| 2 | CTL выполняет запросы | да | да | **да** |
| 3 | DMS: информация о модеме | да | да | **да** |
| 4 | UIM обнаруживает SIM | да | да | **да** (SIM обнаружена, USIM ready) |
| 5 | NAS: состояние сети | да | да | **да** (IN_SERVICE, LTE) |
| 6 | Radio HAL регистрируется | да (7 сервисов) | да | **да** (запущен как сервис, Android его использует) |
| 7 | Android видит Radio service | да (VINTF-фрагмент в `/vendor`) | VINTF/rc не компилировались, ставятся файлами | **да** (init-сервис + VINTF-фрагмент в `/vendor`) |
| 8 | SIM в настройках Android | да (CardStatus с ICCID: DMS, запасной EF_ICCID) | да | **да** (нужен ICCID, иначе подписки нет: `subId=-1`) |
| 9 | Регистрация в сети | да | да | **да** |
| 10 | SMS | да (отправка, приём, ACK) | да | **да**, входящие и исходящие. Модем отдаёт «голый» TPDU без SMSC, HAL добавляет префикс `0x00` |
| 11 | Звонки | да | да | **да**: сигнализация, звук, мут микрофона. DTMF, ответ на второй вызов, конференция — не проверены. Громкость звонка и переключение тихая/громкая не подтверждены |
| 12 | Мобильный интернет | да (WDA raw IP, bind SIO, IPv4 на `wwan0`) | да | **да** (трафик идёт) |
| + | USSD (`*100#` и т. п.) | да (Originate/Answer/Cancel, индикации) | да | **да** |
| + | Автозапуск | init-rc в recovery-zip (+ telephony features, VINTF) | да | **да** (init запускает сервис, телефония поднимается) |
| + | Тихая/громкая из интерфейса Android | нет | — | **не работает**: в audio policy только Speaker, Telecom видит один маршрут |

## Что выяснено на устройстве

- **SMS:** входящий приходит как TPDU без SMSC-префикса. Android ждёт RIL-формат: длина SMSC, SMSC, TPDU. HAL добавляет `0x00`.
- **Данные, BAM-DMUX без QMAP:** интерфейс `wwanN` работает в raw IP. WDA Set Data Format ставит raw IP. Клиент привязывается командой **WDS Bind Data Port 0x0089** с SIO-портом
  `A2_MUX_RMNET0 + N`, для `wwan0` это `0x0E04`; Bind Mux Data Port 0x00A2 модем отклоняет. `wwan0` поднимается **до** Start Network, потому что драйвер шлёт модему OPEN канала.
  **IPv4-адрес на интерфейс ставит RIL**: Android для сотовой сети его не назначает, `netd` добавляет только MTU и маршруты. Без этого пакеты не уходили.
  Счётчики ядра у BAM-DMUX всегда 0, смотреть надо счётчики модема WDS 0x24. HAL пишет строку `data diag` в лог через 20 и 60 с после поднятия вызова.
- **Голос, q6voice:** DSP-сессия стартует, когда открыты оба PCM `pcmC0D4p/c`. Порты выбираются миксерами `PRI_MI2S_RX Voice Mixer CS-Voice` и `CS-Voice Capture Mixer TERT_MI2S_TX`.
  Усилитель динамика искажает после переключения `SPK DAC Switch`, поэтому маршрут выбирается цифровыми `RX1/RX3 Mute Switch`, а `SPK DAC` во время звонка не трогается.
- **Мут микрофона** Android в RIL `setMute` не присылает, он идёт через `AudioManager`. Он виден в логе штатного Audio HAL строкой `AHAL_Module: setMicMute: default: 0|1`, HAL читает её из logcat.
- **Громкость звонка:** индекс `STREAM_VOICE_CALL` из `dumpsys audio` переводится в `RX1/2/3 Digital Volume`. Формула эмпирическая: 60 + 6·индекс−1, максимум 100.
- **USSD:** Originate 0x3A, Answer 0x3B, Cancel 0x3C, индикации 0x3D и 0x3E, кодировки ASCII и UCS2.
- **Telephony features:** на чистом GSI не объявлены `android.hardware.telephony*`. Без них `com.android.phone` вообще не обращается к RIL, в логе нет ни одной строки `RILJ`.
  Recovery-zip ставит `/vendor/etc/permissions/android.hardware.telephony.j5xnlte.xml`.
- **ICCID:** Android 15 не создаёт подписку, `subId=-1` и «no valid subs», если `CardStatus.iccid` пуст. Это пустое меню SIM и «мобильная сеть недоступна» при работающей регистрации.
  HAL читает DMS 0x3C, затем EF_ICCID `2FE2` через UIM.
- **Vendor на `cache`:** recovery-установщик ищет vendor на разделе `vendor`, затем на `cache` по by-name и принимает кандидата, только если в нём есть `bin/hw` и `etc/init`.

## Известные ограничения

- Имена контролов кодека и порты зашиты под J5 2016 (взяты из `tinymix` на этом устройстве); на другом устройстве нужна проверка.
- Мут читается разбором logcat (`AHAL_Module`), громкость — разбором `dumpsys audio`: оба способа хрупкие и зависят от формата вывода Android.
- Один data call, только IPv4.
- Старый `rild` установщики **не отключают**. На установке с Android 15 `vendor.ril-daemon` работает рядом с этим HAL; Android использует наш HAL, но мешает ли штатный rild,
  **не выяснялось**. Намеренно держать два RIL на одном модеме нельзя.
- Android 15 получает `REQUEST_NOT_SUPPORTED` (error 6) на ряд необязательных запросов (`SEND_DEVICE_STATE`, `ENABLE_VONR`, `SET_DATA_PROFILE`): так задумано (не реализовано, «успеха» нет).


## Установка

Готовые zip лежат на странице **Releases** репозитория:

- `j5xnlte-ril-install.zip`: ставит HAL с автозапуском;
- `j5xnlte-ril-uninstall.zip`: удаляет всё установленное и возвращает сохранённые файлы.

Установка из Lineage Recovery: Apply update → Apply from ADB, затем на ПК:
```
adb sideload j5xnlte-ril-install.zip
```
Zip не подписан, recovery спросит, продолжать ли. Он ищет vendor на разделе `vendor`, затем на `cache`, и пишет в `/vendor`: HAL,
`j5x-ril-start.sh`, который подгружает модули модема и перезапускает HAL до 20 раз, `j5xnlte-ril.rc` как init-сервис от root,
VINTF-фрагмент `android.hardware.radio.j5xnlte-qmi.xml` и файл telephony features. Всё, что лежало раньше, сохраняется в `.j5ril-backup`.
Штатный `rild` не отключается.

Удаление: `adb sideload j5xnlte-ril-uninstall.zip`. Выключение без recovery: `setprop persist.vendor.j5ril.enable 0` и перезагрузка.
Установка **проверена на устройстве**: `cache` смонтирован как vendor, файлы установлены, init-сервис работает. Удаление и выключение через свойство на устройстве не проверялись.

Сборка zip самостоятельно: GitHub Actions собирает HAL Android NDK, затем
`bash scripts/make-recovery-zip.sh <бинарник HAL> build/`. Локально HAL собирается `scripts/build-hal-ndk.sh <NDK> <aidl> <aidl_api> <out>`.

## Структура

```
Android.bp                       cc_binary + статическая библиотека QMI/RIL + prebuilt telephony features
android/                         init rc, recovery/ (update-binary, обёртка, rc), vintf, permissions, ueventd, .mk
qmi/                             кодек, транспорт, лог (чистый C, без Android)
ril/core/                        синхронный API модема (ril_qmi.h) и сервисы QMI
hal/src, hal/gen                 реализация HAL (+ voice_audio.cc); базовые классы из замороженного AIDL V4 (tools/gen-hal-base)
tools/gen-hal-base               генератор базовых классов HAL
tools/qmi-probe, prebuilt/       диагностика без libc для aarch64
tools/ril-cli                    консольный клиент настоящего ядра (SMS/звонки/данные/USSD без Android)
tools/voice, tools/diag          вспомогательные скрипты (старый shell-вариант аудио, сбор диагностики)
scripts/                         run-all-tests.sh, build-hal-ndk.sh, make-recovery-zip.sh, install.sh, device/*
tests/                           кодек, транспорт, glue, HAL, install, recovery; поддельные модемы на Python
.github/workflows/ci.yml         сборка HAL NDK
docs/AUDIT_RU.md, device-logs/   аудит TB-qrild/libqril/aospm и выбор V4; логи с устройства
DEPENDENCIES.md, LICENSE, NOTICE зависимости и лицензии
```

## CI

GitHub Actions собирает HAL Android NDK и выкладывает артефакт `j5xnlte-radio-hal-ndk`.
