/* SPDX-License-Identifier: Apache-2.0
 * Standalone NDK build: binder_process.h is a platform (LL-NDK) header, absent from the public NDK. Declarations match the
 * platform header; the functions are provided by tools/ndk-shim/stability_shim.cpp, which resolves them from the real
 * libbinder_ndk.so at run time. Used only through -idirafter, so a real header (AOSP build) wins. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void ABinderProcess_startThreadPool(void);
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
#ifdef __cplusplus
}
#endif
