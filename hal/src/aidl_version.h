// SPDX-License-Identifier: Apache-2.0
// Radio AIDL version the HAL is built against (-DJ5_AIDL_VER=N). Default 4 (Android 16 framework).
// A vendor with API level 34 (Android 14, FCM level 7) has to ship radio V1: build with J5_AIDL_VER=1.
#pragma once
#include <cstdint>
#ifndef J5_AIDL_VER
#define J5_AIDL_VER 4
#endif
namespace j5ril {
// RadioConst::VALUE_UNAVAILABLE* exist only from V4 on; same values.
constexpr int32_t kValueUnavailable = 0x7FFFFFFF;
constexpr int64_t kValueUnavailableLong = 0x7FFFFFFFFFFFFFFFLL;
constexpr uint8_t kValueUnavailableByte = 0xFF;
}  // namespace j5ril
