#pragma once
#include <cstdint>
#include <android/binder_auto_utils.h>
typedef int32_t binder_status_t;
inline binder_status_t AServiceManager_addService(AIBinder*, const char*) { return 0; }
