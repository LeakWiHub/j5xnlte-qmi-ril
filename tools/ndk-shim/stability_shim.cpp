// SPDX-License-Identifier: Apache-2.0
// Platform (LL-NDK) functions that exist in libbinder_ndk.so on the device but not in the public NDK stub used for a
// standalone build. They are resolved from the real library at run time instead of failing the link.
#include <android/binder_ibinder.h>
#include <android/binder_status.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <dlfcn.h>

static void* sym(const char* name) {
    static void* h = nullptr;
    if (!h) h = dlopen("libbinder_ndk.so", RTLD_NOW);
    return h ? dlsym(h, name) : nullptr;
}

extern "C" void AIBinder_markVintfStability(AIBinder* binder) {
    if (auto fn = reinterpret_cast<void (*)(AIBinder*)>(sym("AIBinder_markVintfStability"))) fn(binder);
}
extern "C" void AIBinder_markCompilationUnitStability(AIBinder* binder) {
    if (auto fn = reinterpret_cast<void (*)(AIBinder*)>(sym("AIBinder_markCompilationUnitStability"))) fn(binder);
}
extern "C" binder_status_t AServiceManager_addService(AIBinder* binder, const char* instance) {
    auto fn = reinterpret_cast<binder_status_t (*)(AIBinder*, const char*)>(sym("AServiceManager_addService"));
    return fn ? fn(binder, instance) : STATUS_NAME_NOT_FOUND;
}
extern "C" void ABinderProcess_startThreadPool(void) {
    if (auto fn = reinterpret_cast<void (*)()>(sym("ABinderProcess_startThreadPool"))) fn();
}
extern "C" bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t n) {
    auto fn = reinterpret_cast<bool (*)(uint32_t)>(sym("ABinderProcess_setThreadPoolMaxThreadCount"));
    return fn ? fn(n) : false;
}
extern "C" void ABinderProcess_joinThreadPool(void) {
    if (auto fn = reinterpret_cast<void (*)()>(sym("ABinderProcess_joinThreadPool"))) fn();
}
