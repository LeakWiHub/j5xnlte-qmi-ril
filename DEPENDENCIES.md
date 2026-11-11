# Зависимости и лицензии

Код проекта — Apache-2.0 (`LICENSE`, SPDX-заголовки).

## Во время выполнения (внутри образа)
| Что | Откуда | Лицензия |
|---|---|---|
| libbase, libbinder_ndk, liblog | AOSP, vendor-доступные (VNDK) | Apache-2.0 |
| `android.hardware.radio{,.config,.modem,.sim,.network,.messaging,.voice,.data}-V4-ndk` | собираются из AIDL в дереве AOSP/Lineage | Apache-2.0 |
| pthread/POSIX (bionic) | платформа | — |

QRTR, libqrtr, libqril, qmic во время выполнения **не используются**.

## Для сборки на хосте / тестов
gcc, g++ (ASan/UBSan/TSan), clang (синтаксическая проверка), Python 3 (фейк-модемы, генераторы), опционально qemu-user и
aarch64-кросс-компилятор (probe). Для HAL-теста нужен каталог `hardware/interfaces/radio/aidl/aidl_api` из LineageOS 23.0.

## Справочные материалы (код НЕ скопирован)
| Источник | Использование | Лицензия |
|---|---|---|
| TB-X505X-Labs/android_external_qrild, android_external_libqril (lineage-24.0) | сверка раскладки QMI-сообщений (IDL), идея базовых классов с `REQUEST_NOT_SUPPORTED`, два реальных кадра в тесте кодека | файла лицензии нет; в radio.cc заявлен SPDX Apache-2.0 |
| aospm/qrild, aospm/libqril | чтение | файла лицензии нет |
| aospm/qrtr | чтение | BSD-стиль, © 2015 Sony Mobile Communications Inc. |
| aospm/qmic | чтение | BSD-стиль, © 2016 Linaro Ltd |
| LineageOS/android_hardware_interfaces, android_frameworks_native (lineage-23.0) | замороженный AIDL V4; `hal/gen/*.h` генерируется из него (производный код, Apache-2.0) | Apache-2.0 |

Если вы захотите перенести код из репозиториев без файла лицензии — сначала уточните лицензию у авторов.
