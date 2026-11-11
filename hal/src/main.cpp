// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "j5xnlte-ril"
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstddef>

#include <cstdlib>
#include <cstring>

#include "radio_impl.h"

using j5ril::Hal;

static bool add(const std::shared_ptr<ndk::ICInterface>& svc, const std::string& descriptor, const char* instance) {
    std::string name = descriptor + "/" + instance;
    binder_status_t st = AServiceManager_addService(svc->asBinder().get(), name.c_str());
    if (st != 0) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "cannot register %s: %d", name.c_str(), static_cast<int>(st));
    else __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "registered %s", name.c_str());
    return st == 0;
}

int main(int argc, char** argv) {
    /* default node can be overridden for bring-up: android.hardware.radio-service.j5xnlte-qmi /dev/other */
    const char* dev = argc > 1 ? argv[1] : "/dev/wwan0qmi0";

    /* Only one RIL may drive the modem: bind an abstract unix socket as a process-wide singleton lock (no files, the
     * kernel drops it when we die). A second copy of this service exits instead of fighting the first one. Another
     * vendor rild is kept away by the init rc, see README_RU.md. */
    int lk = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    static const char kLock[] = "j5xnlte-ril-singleton";
    memcpy(sa.sun_path + 1, kLock, sizeof(kLock) - 1); /* leading NUL = abstract namespace */
    if (lk < 0 || bind(lk, reinterpret_cast<sockaddr*>(&sa), offsetof(sockaddr_un, sun_path) + 1 + sizeof(kLock) - 1) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "another instance already owns the modem (or no socket), exiting");
        return 1;
    }

    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    static Hal hal;
    if (!hal.start(dev)) {
        __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, "cannot start the QMI stack on %s", dev);
        return 1;
    }
    bool ok = true;
    ok &= add(hal.config, "android.hardware.radio.config.IRadioConfig", "default");
    ok &= add(hal.modem, "android.hardware.radio.modem.IRadioModem", "slot1");
    ok &= add(hal.sim, "android.hardware.radio.sim.IRadioSim", "slot1");
    ok &= add(hal.network, "android.hardware.radio.network.IRadioNetwork", "slot1");
    ok &= add(hal.messaging, "android.hardware.radio.messaging.IRadioMessaging", "slot1");
    ok &= add(hal.voice, "android.hardware.radio.voice.IRadioVoice", "slot1");
    ok &= add(hal.data, "android.hardware.radio.data.IRadioData", "slot1");
    if (!ok) return 1;

    ABinderProcess_joinThreadPool();
    return 1; /* joinThreadPool never returns in normal operation */
}
