/* SPDX-License-Identifier: Apache-2.0
 * Standalone NDK build: binder_manager.h is a platform (LL-NDK) header, absent from the public NDK (see binder_process.h). */
#pragma once
#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#ifdef __cplusplus
extern "C" {
#endif
binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance);
#ifdef __cplusplus
}
#endif
