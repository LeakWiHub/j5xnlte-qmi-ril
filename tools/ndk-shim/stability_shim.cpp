// SPDX-License-Identifier: Apache-2.0
// The stability functions are in libbinder_ndk.so on the device (LL-NDK) but not in the NDK stub used for a standalone
// build. Resolve them from the real library at run time instead of failing the link.
#include <android/binder_ibinder.h>
#include <dlfcn.h>

static void call_real(const char* name, AIBinder* b) {
    static void* h = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!h) return;
    auto fn = reinterpret_cast<void (*)(AIBinder*)>(dlsym(h, name));
    if (fn) fn(b);
}

extern "C" void AIBinder_markVintfStability(AIBinder* binder) { call_real("AIBinder_markVintfStability", binder); }
extern "C" void AIBinder_markCompilationUnitStability(AIBinder* binder) { call_real("AIBinder_markCompilationUnitStability", binder); }
