// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "j5xnlte-ril"
#include <android/log.h>

#include <cerrno>
#include <cstdio>

#include "radio_impl.h"

namespace j5ril {

/* ------------------------------------------------------------------ Executor */
Executor::Executor(int threads) {
    for (int i = 0; i < threads; i++) th_.emplace_back([this] { loop(); });
}

Executor::~Executor() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : th_) t.join();
}

void Executor::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> l(mu_);
        q_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void Executor::loop() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) return;
            fn = std::move(q_.front());
            q_.pop_front();
        }
        fn();
    }
}

/* ------------------------------------------------------------------ helpers */
radio::RadioError mapError(int rc) {
    using radio::RadioError;
    if (rc == 0) return RadioError::NONE;
    if (rc == -ENOTCONN) return RadioError::RADIO_NOT_AVAILABLE;
    if (rc == -ENOTSUP) return RadioError::REQUEST_NOT_SUPPORTED;
    if (rc == -EINVAL || rc == -ENAMETOOLONG || rc == -EMSGSIZE) return RadioError::INVALID_ARGUMENTS;
    if (rc == -ETIMEDOUT) return RadioError::MODEM_ERR;
    if (rc == -EBUSY) return RadioError::NO_RESOURCES;
    if (QMI_E_IS_QMI(rc)) {
        switch (QMI_E_CODE(rc)) {
            case QMI_ERR_INCORRECT_PIN:
            case QMI_ERR_PIN_BLOCKED:
            case QMI_ERR_PIN_PERM_BLOCKED:
                return RadioError::PASSWORD_INCORRECT; /* the framework reads the remaining retries / re-reads the card state */
            case QMI_ERR_NOT_PROVISIONED:
                return RadioError::SUBSCRIPTION_NOT_AVAILABLE;
            case QMI_ERR_UIM_NOT_INITIALIZED:
                return RadioError::SIM_BUSY;
            case QMI_ERR_INVALID_QMI_CMD:
            case QMI_ERR_OP_DEVICE_UNSUPPORTED:
                return RadioError::REQUEST_NOT_SUPPORTED;
            case QMI_ERR_NO_NETWORK_FOUND:
                return RadioError::OP_NOT_ALLOWED_BEFORE_REG_TO_NW;
            default:
                return RadioError::GENERIC_FAILURE;
        }
    }
    return RadioError::GENERIC_FAILURE;
}

std::string toHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

bool fromHex(const std::string& s, std::vector<uint8_t>* out) {
    out->clear();
    if (s.size() % 2) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out->push_back(static_cast<uint8_t>(a * 16 + b));
    }
    return true;
}

/* QMI reports MCC/MNC as numbers, which loses the leading zeros of the MNC. Three digit MNCs are used by the
 * countries below (ITU-T E.212 / GSMA list, not exhaustive); everywhere else two digits are assumed. */
std::string plmnString(int mcc, int mnc) {
    static const int kThreeDigit[] = {302, 310, 311, 312, 313, 314, 315, 316, 334, 338, 342, 344, 346, 348, 350, 352, 354,
                                      356, 358, 360, 364, 365, 366, 368, 370, 372, 374, 376, 405, 708, 722, 732, 750};
    bool three = mnc >= 100;
    for (int m : kThreeDigit)
        if (m == mcc) three = true;
    char b[16];
    snprintf(b, sizeof(b), three ? "%03d%03d" : "%03d%02d", mcc, mnc);
    return b;
}

static void qmiLog(int level, const char* msg) {
    int p = level == QMI_LOG_ERROR ? ANDROID_LOG_ERROR
            : level == QMI_LOG_WARN ? ANDROID_LOG_WARN
            : level == QMI_LOG_INFO ? ANDROID_LOG_INFO
                                    : ANDROID_LOG_DEBUG;
    __android_log_print(p, LOG_TAG, "%s", msg);
}

void installQmiLogger() { qmi_set_logger(qmiLog, QMI_LOG_INFO); }

/* ------------------------------------------------------------------ Hal */
Hal::Hal() {
    config = ndk::SharedRefBase::make<RadioConfigImpl>(*this);
    modem = ndk::SharedRefBase::make<RadioModemImpl>(*this);
    sim = ndk::SharedRefBase::make<RadioSimImpl>(*this);
    network = ndk::SharedRefBase::make<RadioNetworkImpl>(*this);
    messaging = ndk::SharedRefBase::make<RadioMessagingImpl>(*this);
    voice = ndk::SharedRefBase::make<RadioVoiceImpl>(*this);
    data = ndk::SharedRefBase::make<RadioDataImpl>(*this);
}

Hal::~Hal() { stop(); }

/* Callbacks arrive on the RIL worker thread (never the QMI reader). */
static Hal* H(void* ctx) { return static_cast<Hal*>(ctx); }

static void evModemState(void* c, int up) {
    Hal* h = H(c);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "modem %s", up ? "ready" : "lost");
    if (up) {
        int mode = -1;
        if (ril_modem_get_opmode(h->ril, &mode) == 0) {
            std::lock_guard<std::mutex> l(h->mu);
            h->radioOn = mode == RIL_OPMODE_ONLINE ? 1 : 0;
        }
    } else {
        std::lock_guard<std::mutex> l(h->mu);
        h->radioOn = -1;
        h->dataCall.active = false; /* the modem lost the session with the transport */
    }
    h->modem->notifyRadioState();
    h->sim->notifySimChanged();
    h->config->notifySlotsChanged();
}
static void evSim(void* c) {
    H(c)->sim->notifySimChanged();
    H(c)->config->notifySlotsChanged();
}
static void evNet(void* c) { H(c)->network->notifyNetworkChanged(); }
static void evSig(void* c) { H(c)->network->notifySignalChanged(); }
static void evSms(void* c, const uint8_t* pdu, size_t len, int) { H(c)->messaging->notifyNewSms(std::vector<uint8_t>(pdu, pdu + len)); }
static void evCalls(void* c) { H(c)->voice->notifyCallsChanged(); }
static void evData(void* c, int up) { H(c)->data->notifyDataStatus(up != 0); }

bool Hal::start(const char* dev) {
    installQmiLogger();
    ril_events_t ev = {evModemState, evSim, evNet, evSig, evSms, evCalls, evData, this};
    ril = ril_qmi_create(dev, &ev);
    return ril != nullptr;
}

void Hal::stop() {
    if (ril) {
        ril_qmi_destroy(ril);
        ril = nullptr;
    }
}

}  // namespace j5ril
