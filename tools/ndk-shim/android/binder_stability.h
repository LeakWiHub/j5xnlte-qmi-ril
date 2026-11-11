/* SPDX-License-Identifier: Apache-2.0
 * Fallback for a standalone NDK build: the real header is not part of the public NDK. Only used when the NDK does not
 * ship it (-idirafter). The symbol itself is resolved at run time from libbinder_ndk.so by tools/ndk-shim/stability_shim.cpp. */
#pragma once
#include <android/binder_ibinder.h>
#ifdef __cplusplus
extern "C" {
#endif
void AIBinder_markVintfStability(AIBinder* binder);
void AIBinder_markCompilationUnitStability(AIBinder* binder);
#ifdef __cplusplus
}
#endif
