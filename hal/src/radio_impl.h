// SPDX-License-Identifier: Apache-2.0
// Radio AIDL V4 HAL for the Samsung Galaxy J5 2016 (j5xnlte, MSM8916) on top of the userspace QMI stack.
#pragma once
#include "aidl_version.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "ril_qmi.h"
}

#include "RadioConfigBase.h"
#include "RadioDataBase.h"
#include "RadioMessagingBase.h"
#include "RadioModemBase.h"
#include "RadioNetworkBase.h"
#include "RadioSimBase.h"
#include "RadioVoiceBase.h"

namespace j5ril {

namespace radio = ::aidl::android::hardware::radio;

/* Binder threads must never sit in a blocking QMI call for long, so every request is queued here. The queue is
 * FIFO and requests start in submission order; they may complete out of order (the QMI layer is thread safe). */
class Executor {
  public:
    explicit Executor(int threads);
    ~Executor();
    void post(std::function<void()> fn);

  private:
    void loop();
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> th_;
    bool stop_ = false;
};

class Hal;

class RadioConfigImpl : public RadioConfigBase {
  public:
    explicit RadioConfigImpl(Hal& h) : h_(h) {}
    ::ndk::ScopedAStatus getHalDeviceCapabilities(int32_t in_serial) override;
    ::ndk::ScopedAStatus getNumOfLiveModems(int32_t in_serial) override;
    ::ndk::ScopedAStatus getPhoneCapability(int32_t in_serial) override;
    ::ndk::ScopedAStatus getSimSlotsStatus(int32_t in_serial) override;
    ::ndk::ScopedAStatus setPreferredDataModem(int32_t in_serial, int8_t in_modemId) override;
    void notifySlotsChanged();

  private:
    std::vector<radio::config::SimSlotStatus> slots();
    Hal& h_;
};

class RadioModemImpl : public RadioModemBase {
  public:
    explicit RadioModemImpl(Hal& h) : h_(h) {}
    ::ndk::ScopedAStatus getBasebandVersion(int32_t in_serial) override;
    ::ndk::ScopedAStatus getDeviceIdentity(int32_t in_serial) override;
#if J5_AIDL_VER >= 2
    ::ndk::ScopedAStatus getImei(int32_t in_serial) override;
#endif
    ::ndk::ScopedAStatus getHardwareConfig(int32_t in_serial) override;
    ::ndk::ScopedAStatus setRadioPower(int32_t in_serial, bool in_powerOn, bool in_forEmergencyCall,
                                       bool in_preferredForEmergencyCall) override;
    ::ndk::ScopedAStatus requestShutdown(int32_t in_serial) override;
    void notifyRadioState();

  protected:
    void onResponseFunctionsSet() override;

  private:
    Hal& h_;
};

class RadioSimImpl : public RadioSimBase {
  public:
    explicit RadioSimImpl(Hal& h) : h_(h) {}
    ::ndk::ScopedAStatus getIccCardStatus(int32_t in_serial) override;
    ::ndk::ScopedAStatus iccIoForApp(int32_t in_serial, const radio::sim::IccIo& in_iccIo) override;
    ::ndk::ScopedAStatus supplyIccPinForApp(int32_t in_serial, const std::string& in_pin, const std::string& in_aid) override;
    ::ndk::ScopedAStatus supplyIccPin2ForApp(int32_t in_serial, const std::string& in_pin2, const std::string& in_aid) override;
    ::ndk::ScopedAStatus supplyIccPukForApp(int32_t in_serial, const std::string& in_puk, const std::string& in_pin,
                                            const std::string& in_aid) override;
    ::ndk::ScopedAStatus supplyIccPuk2ForApp(int32_t in_serial, const std::string& in_puk2, const std::string& in_pin2,
                                             const std::string& in_aid) override;
    ::ndk::ScopedAStatus changeIccPinForApp(int32_t in_serial, const std::string& in_oldPin, const std::string& in_newPin,
                                            const std::string& in_aid) override;
    ::ndk::ScopedAStatus changeIccPin2ForApp(int32_t in_serial, const std::string& in_oldPin2, const std::string& in_newPin2,
                                             const std::string& in_aid) override;
    ::ndk::ScopedAStatus getImsiForApp(int32_t in_serial, const std::string& in_aid) override;
    ::ndk::ScopedAStatus getFacilityLockForApp(int32_t in_serial, const std::string& in_facility, const std::string& in_password,
                                               int32_t in_serviceClass, const std::string& in_appId) override;
    ::ndk::ScopedAStatus setFacilityLockForApp(int32_t in_serial, const std::string& in_facility, bool in_lockState,
                                               const std::string& in_password, int32_t in_serviceClass,
                                               const std::string& in_appId) override;
    void notifySimChanged();
    bool cardStatus(radio::sim::CardStatus* out, radio::RadioError* err);

  private:
    template <class F> void pinOp(int32_t serial, F op, int kind);
    Hal& h_;
};

class RadioNetworkImpl : public RadioNetworkBase {
  public:
    explicit RadioNetworkImpl(Hal& h) : h_(h) {}
    ::ndk::ScopedAStatus getVoiceRegistrationState(int32_t in_serial) override;
    ::ndk::ScopedAStatus getDataRegistrationState(int32_t in_serial) override;
    ::ndk::ScopedAStatus getOperator(int32_t in_serial) override;
    ::ndk::ScopedAStatus getSignalStrength(int32_t in_serial) override;
    ::ndk::ScopedAStatus getVoiceRadioTechnology(int32_t in_serial) override;
    ::ndk::ScopedAStatus getNetworkSelectionMode(int32_t in_serial) override;
    ::ndk::ScopedAStatus setNetworkSelectionModeAutomatic(int32_t in_serial) override;
    ::ndk::ScopedAStatus setNetworkSelectionModeManual(int32_t in_serial, const std::string& in_operatorNumeric,
                                                       radio::AccessNetwork in_ran) override;
    ::ndk::ScopedAStatus getAvailableNetworks(int32_t in_serial) override;
    ::ndk::ScopedAStatus getAllowedNetworkTypesBitmap(int32_t in_serial) override;
    ::ndk::ScopedAStatus setAllowedNetworkTypesBitmap(int32_t in_serial, int32_t in_networkTypeBitmap) override;
    ::ndk::ScopedAStatus setIndicationFilter(int32_t in_serial, int32_t in_indicationFilter) override;
    ::ndk::ScopedAStatus setSignalStrengthReportingCriteria(
            int32_t in_serial, const std::vector<radio::network::SignalThresholdInfo>& in_signalThresholdInfos) override;
    void notifyNetworkChanged();
    void notifySignalChanged();

  private:
    radio::network::RegStateResult regState(const ril_serving_t& s, bool data);
    static radio::network::SignalStrength signalFrom(const ril_signal_t& s);
    Hal& h_;
};

class RadioMessagingImpl : public RadioMessagingBase {
  public:
    explicit RadioMessagingImpl(Hal& h) : h_(h) {}
    ::ndk::ScopedAStatus sendSms(int32_t in_serial, const radio::messaging::GsmSmsMessage& in_message) override;
    ::ndk::ScopedAStatus sendSmsExpectMore(int32_t in_serial, const radio::messaging::GsmSmsMessage& in_message) override;
    ::ndk::ScopedAStatus acknowledgeLastIncomingGsmSms(int32_t in_serial, bool in_success,
                                                       radio::messaging::SmsAcknowledgeFailCause in_cause) override;
    ::ndk::ScopedAStatus acknowledgeIncomingGsmSmsWithPdu(int32_t in_serial, bool in_success, const std::string& in_ackPdu) override;
    void notifyNewSms(const std::vector<uint8_t>& pdu);

  private:
    void send(int32_t serial, const radio::messaging::GsmSmsMessage& m, bool expectMore);
    Hal& h_;
};

class RadioVoiceImpl : public RadioVoiceBase {
  public:
    explicit RadioVoiceImpl(Hal& h) : h_(h) {}
    ::ndk::ScopedAStatus getCurrentCalls(int32_t in_serial) override;
    ::ndk::ScopedAStatus dial(int32_t in_serial, const radio::voice::Dial& in_dialInfo) override;
    ::ndk::ScopedAStatus emergencyDial(int32_t in_serial, const radio::voice::Dial& in_dialInfo, int32_t in_categories,
                                       const std::vector<std::string>& in_urns, radio::voice::EmergencyCallRouting in_routing,
                                       bool in_hasKnownUserIntentEmergency, bool in_isTesting) override;
    ::ndk::ScopedAStatus acceptCall(int32_t in_serial) override;
    ::ndk::ScopedAStatus rejectCall(int32_t in_serial) override;
    ::ndk::ScopedAStatus hangup(int32_t in_serial, int32_t in_gsmIndex) override;
    ::ndk::ScopedAStatus hangupWaitingOrBackground(int32_t in_serial) override;
    ::ndk::ScopedAStatus sendDtmf(int32_t in_serial, const std::string& in_s) override;
    ::ndk::ScopedAStatus startDtmf(int32_t in_serial, const std::string& in_s) override;
    ::ndk::ScopedAStatus stopDtmf(int32_t in_serial) override;
    ::ndk::ScopedAStatus setMute(int32_t in_serial, bool in_enable) override;
    ::ndk::ScopedAStatus getMute(int32_t in_serial) override;
    ::ndk::ScopedAStatus getLastCallFailCause(int32_t in_serial) override;
    ::ndk::ScopedAStatus sendUssd(int32_t in_serial, const std::string& in_ussd) override;
    ::ndk::ScopedAStatus cancelPendingUssd(int32_t in_serial) override;
    void notifyCallsChanged();
    /* type = UssdModeType value (0 NOTIFY, 1 REQUEST, 2 NW_RELEASE) */
    void notifyUssd(int type, const std::string& text);

  private:
    struct Snapshot {
        std::vector<ril_call_t> calls;
        std::vector<int> index; /* Android call index (1..7) per call */
    };
    bool snapshot(Snapshot* out, radio::RadioError* err);
    int idForIndex(int index);
    int activeId();
    void dialCommon(int32_t serial, const std::string& number, bool emergency);
    Hal& h_;
    std::mutex mu_;
    std::map<int, int> idToIndex_; /* QMI call id -> stable Android index */
    bool ussdPending_ = false;     /* Android sent a USSD request and has not seen the answer yet */
    std::string lastUssd_;         /* the same text may come both in the response and as an indication */
    std::chrono::steady_clock::time_point lastUssdAt_{};
};

class RadioDataImpl : public RadioDataBase {
  public:
    explicit RadioDataImpl(Hal& h) : h_(h) {}
    ::ndk::ScopedAStatus setupDataCall(int32_t in_serial, radio::AccessNetwork in_accessNetwork,
                                       const radio::data::DataProfileInfo& in_dataProfileInfo, bool in_roamingAllowed,
                                       radio::data::DataRequestReason in_reason,
                                       const std::vector<radio::data::LinkAddress>& in_addresses,
                                       const std::vector<std::string>& in_dnses, int32_t in_pduSessionId,
                                       const std::optional<radio::data::SliceInfo>& in_sliceInfo,
                                       bool in_matchAllRuleAllowed) override;
    ::ndk::ScopedAStatus deactivateDataCall(int32_t in_serial, int32_t in_cid, radio::data::DataRequestReason in_reason) override;
    ::ndk::ScopedAStatus getDataCallList(int32_t in_serial) override;
    ::ndk::ScopedAStatus setDataAllowed(int32_t in_serial, bool in_allow) override;
    void notifyDataStatus(bool connected);

  private:
    Hal& h_;
};

/* Owns the QMI stack and the seven services; routes modem events to the indication callbacks. */
class Hal {
  public:
    Hal();
    ~Hal();
    bool start(const char* dev);
    void stop();

    ril_qmi_t* ril = nullptr;
    Executor exec{4};

    std::shared_ptr<RadioConfigImpl> config;
    std::shared_ptr<RadioModemImpl> modem;
    std::shared_ptr<RadioSimImpl> sim;
    std::shared_ptr<RadioNetworkImpl> network;
    std::shared_ptr<RadioMessagingImpl> messaging;
    std::shared_ptr<RadioVoiceImpl> voice;
    std::shared_ptr<RadioDataImpl> data;

    /* state shared between the services (guarded by mu) */
    std::mutex mu;
    int32_t indicationFilter = ~0;
    bool dataAllowed = true;
    struct ActiveCall {
        bool active = false;
        ril_data_call_t dc{};
    } dataCall;
    int radioOn = -1; /* -1 unknown, 0 low power, 1 online */

    bool ready() { return ril && ril_qmi_ready(ril); }
};

/* ---- helpers shared by the services ---- */
radio::RadioError mapError(int rc);
std::string toHex(const uint8_t* p, size_t n);
bool fromHex(const std::string& s, std::vector<uint8_t>* out);
std::string plmnString(int mcc, int mnc);
void installQmiLogger();

}  // namespace j5ril
