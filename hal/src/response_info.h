// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <aidl/android/hardware/radio/RadioError.h>
#include <aidl/android/hardware/radio/RadioResponseInfo.h>
#include <aidl/android/hardware/radio/RadioResponseType.h>
#include <aidl/android/hardware/radio/RadioResponseInfoModem.h>

using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioResponseInfo;
using ::aidl::android::hardware::radio::RadioResponseType;
using ::aidl::android::hardware::radio::RadioResponseInfoModem;

inline RadioResponseInfo rspInfo(int32_t serial, RadioError err = RadioError::NONE) {
    RadioResponseInfo i;
    i.type = RadioResponseType::SOLICITED;
    i.serial = serial;
    i.error = err;
    return i;
}

inline RadioResponseInfo notSupported(int32_t serial) { return rspInfo(serial, RadioError::REQUEST_NOT_SUPPORTED); }

inline RadioResponseInfoModem modemInfo(int32_t serial, RadioError err = RadioError::NONE) {
    RadioResponseInfoModem i;
    i.type = RadioResponseType::SOLICITED;
    i.serial = serial;
    i.error = err;
    return i;
}

inline RadioResponseInfoModem modemNotSupported(int32_t serial) { return modemInfo(serial, RadioError::REQUEST_NOT_SUPPORTED); }
