# Аудит источников (проверено по клонированным репозиториям)

| Репозиторий | Ветка / HEAD | Что установлено |
|---|---|---|
| TB-X505X-Labs/android_external_qrild | lineage-24.0 @ d4ff781 (2026-10-16), 162 коммита | Свои копии qrtr и libqril внутри дерева. Транспорт только `socket(AF_QIPCRTR)`. Radio AIDL **V5** (`-V5-ndk`). |
| TB-X505X-Labs/android_external_libqril | lineage-24.0 @ e698dc6, 5 коммитов | Отдельная libqril; не равна встроенной копии в qrild, подменять одно другим нельзя. |
| aospm/qrild | rewrite @ 28a46d8 (2022-10-06) | Незавершённый переход на standalone libqril. |
| zg4hbczp6q-lang/We-rheiqjejeiekw | Dev @ 28a46d8 | **Дерево побайтно совпадает с aospm/qrild rewrite** (`diff -rq` без различий). Собственных коммитов поверх upstream нет. |
| aospm/libqril, qrtr, qmic | main/master-ветки | Лицензия есть только у qrtr и qmic. |

## Что реально реализовано в TB-qrild (src/radio.cc, 588 строк)
- Реальные вызовы QMI: Config (слоты), Modem (IMEI, версия, питание), SIM (статус карты, IMSI, ICCID), Network (регистрация, оператор, сигнал), Messaging (отправка/приём SMS, SMSC).
- **Voice и Data зарегистрированы как пустые базовые классы** (`RadioVoiceBase`, `RadioDataBase`): все методы отвечают `REQUEST_NOT_SUPPORTED`. Звонков и мобильного интернета там нет.
- Заглушки с «успехом»: `enableModem` (ничего не делает), `setNetworkSelectionModeAutomatic`, `setIndicationFilter`, `getFacilityLockForApp` (константа 0), `getCellInfoList` (пустой список).
- Транспорт нигде не использует `/dev/wwan*`; QMUX-заголовок для QRTR синтезирует libqrtr. Поэтому символьное устройство `/dev/wwan0qmi0` в этих проектах не поддержано вовсе.

## Выбор версии Radio AIDL
Источник: LineageOS/android_hardware_interfaces, ветка lineage-23.0, коммит 24422e9 (2025-12-20).
- Замороженные версии всех radio-пакетов: **1, 2, 3, 4** (+ `current`; в Android.bp `frozen: false`, то есть «V5» это ещё не замороженная разработка).
- compatibility_matrix.202504.xml (уровень Android 16): `radio.config/data/messaging/modem/network/sim/voice` = **`3-4`**.
- compatibility_matrix.202604.xml: `3-5`. Это матрица следующего релиза, V5 в неё попадает только там.
- **Выбор: V4** — максимальная версия, допустимая для 202504. V5 из TB-qrild автоматически не подходит.
- Отличия V3→V4: `IRadioConfig.getSimTypeInfo/setSimType`; `IRadioNetwork.setSatellitePlmn/setSatelliteEnabledForCarrier/isSatelliteEnabledForCarrier`; константы `RadioConst.VALUE_UNAVAILABLE*`; значения по умолчанию в структурах. Новые методы HAL должен реализовать (честный ответ `REQUEST_NOT_SUPPORTED`).
- Ограничение: проверено по исходникам ветки, а не по вашему устройству. Проверка на устройстве:

```
adb shell 'for f in /system/etc/vintf/compatibility_matrix*.xml /system_ext/etc/vintf/*.xml; do echo "== $f"; grep -B1 -A3 "android.hardware.radio" "$f" | grep -E "<name>|<version>"; done 2>/dev/null'
```
