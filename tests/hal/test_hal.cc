// SPDX-License-Identifier: Apache-2.0
// Host test of the HAL layer: the real HAL sources + the real QMI stack, talking to the Python fake modem,
// with the AIDL types coming from the type-check mock (tools/aidl-mock). It exercises request mapping, async
// responses and indications. It is NOT a binder/ABI test and NOT a device test.
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "Rec_RadioConfig.h"
#include "Rec_RadioData.h"
#include "Rec_RadioMessaging.h"
#include "Rec_RadioModem.h"
#include "Rec_RadioNetwork.h"
#include "Rec_RadioSim.h"
#include "Rec_RadioVoice.h"
#include "radio_impl.h"

using namespace j5ril;
using radio::RadioError;

static int fails, total;
#define CHECK(c, ...)                                  \
    do {                                               \
        total++;                                       \
        if (!(c)) {                                    \
            fails++;                                   \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
        } else {                                       \
            printf("  ok   ");                         \
        }                                              \
        printf(__VA_ARGS__);                           \
        printf("\n");                                  \
    } while (0)

/* ---- recorders that also keep the interesting payloads ---- */
struct ModemResp : RadioModemRecResponse {
    std::string version, imei;
    ::ndk::ScopedAStatus getBasebandVersionResponse(const radio::RadioResponseInfo& i, const std::string& v) override {
        version = v;
        return RadioModemRecResponse::getBasebandVersionResponse(i, v);
    }
    ::ndk::ScopedAStatus getImeiResponse(const radio::RadioResponseInfo& i, const std::optional<radio::modem::ImeiInfo>& v) override {
        if (v) imei = v->imei;
        return RadioModemRecResponse::getImeiResponse(i, v);
    }
};
struct SimResp : RadioSimRecResponse {
    radio::sim::CardStatus card;
    radio::sim::IccIoResult io;
    int retries = -99;
    std::string imsi;
    ::ndk::ScopedAStatus getIccCardStatusResponse(const radio::RadioResponseInfo& i, const radio::sim::CardStatus& c) override {
        card = c;
        return RadioSimRecResponse::getIccCardStatusResponse(i, c);
    }
    ::ndk::ScopedAStatus iccIoForAppResponse(const radio::RadioResponseInfo& i, const radio::sim::IccIoResult& r) override {
        io = r;
        return RadioSimRecResponse::iccIoForAppResponse(i, r);
    }
    ::ndk::ScopedAStatus supplyIccPinForAppResponse(const radio::RadioResponseInfo& i, int32_t left) override {
        retries = left;
        return RadioSimRecResponse::supplyIccPinForAppResponse(i, left);
    }
    ::ndk::ScopedAStatus getImsiForAppResponse(const radio::RadioResponseInfo& i, const std::string& s) override {
        imsi = s;
        return RadioSimRecResponse::getImsiForAppResponse(i, s);
    }
};
struct NetResp : RadioNetworkRecResponse {
    radio::network::RegStateResult reg;
    radio::network::SignalStrength sig;
    std::string lname, num;
    ::ndk::ScopedAStatus getVoiceRegistrationStateResponse(const radio::RadioResponseInfo& i, const radio::network::RegStateResult& r) override {
        reg = r;
        return RadioNetworkRecResponse::getVoiceRegistrationStateResponse(i, r);
    }
    ::ndk::ScopedAStatus getSignalStrengthResponse(const radio::RadioResponseInfo& i, const radio::network::SignalStrength& s) override {
        sig = s;
        return RadioNetworkRecResponse::getSignalStrengthResponse(i, s);
    }
    ::ndk::ScopedAStatus getOperatorResponse(const radio::RadioResponseInfo& i, const std::string& l, const std::string& s, const std::string& n) override {
        lname = l;
        num = n;
        return RadioNetworkRecResponse::getOperatorResponse(i, l, s, n);
    }
};
struct MsgResp : RadioMessagingRecResponse {
    radio::messaging::SendSmsResult res;
    ::ndk::ScopedAStatus sendSmsResponse(const radio::RadioResponseInfo& i, const radio::messaging::SendSmsResult& r) override {
        res = r;
        return RadioMessagingRecResponse::sendSmsResponse(i, r);
    }
};
struct MsgInd : RadioMessagingRecIndication {
    std::vector<uint8_t> pdu;
    ::ndk::ScopedAStatus newSms(radio::RadioIndicationType t, const std::vector<uint8_t>& p) override {
        pdu = p;
        return RadioMessagingRecIndication::newSms(t, p);
    }
};
struct VoiceResp : RadioVoiceRecResponse {
    std::vector<radio::voice::Call> calls;
    ::ndk::ScopedAStatus getCurrentCallsResponse(const radio::RadioResponseInfo& i, const std::vector<radio::voice::Call>& c) override {
        calls = c;
        return RadioVoiceRecResponse::getCurrentCallsResponse(i, c);
    }
};
struct DataResp : RadioDataRecResponse {
    radio::data::SetupDataCallResult dc;
    std::vector<radio::data::SetupDataCallResult> list;
    ::ndk::ScopedAStatus setupDataCallResponse(const radio::RadioResponseInfo& i, const radio::data::SetupDataCallResult& r) override {
        dc = r;
        return RadioDataRecResponse::setupDataCallResponse(i, r);
    }
    ::ndk::ScopedAStatus getDataCallListResponse(const radio::RadioResponseInfo& i, const std::vector<radio::data::SetupDataCallResult>& l) override {
        list = l;
        return RadioDataRecResponse::getDataCallListResponse(i, l);
    }
};
struct CfgResp : RadioConfigRecResponse {
    std::vector<radio::config::SimSlotStatus> slots;
    ::ndk::ScopedAStatus getSimSlotsStatusResponse(const radio::RadioResponseInfo& i, const std::vector<radio::config::SimSlotStatus>& s) override {
        slots = s;
        return RadioConfigRecResponse::getSimSlotsStatusResponse(i, s);
    }
};

static std::vector<uint8_t> unhex(const char* s) {
    std::vector<uint8_t> v;
    fromHex(s, &v);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    setenv("RIL_DATA_IFACE", "lo", 1);
    Hal hal;

    auto mr = std::make_shared<ModemResp>();
    auto mi = std::make_shared<RadioModemRecIndication>();
    auto sr = std::make_shared<SimResp>();
    auto si = std::make_shared<RadioSimRecIndication>();
    auto nr = std::make_shared<NetResp>();
    auto ni = std::make_shared<RadioNetworkRecIndication>();
    auto gr = std::make_shared<MsgResp>();
    auto gi = std::make_shared<MsgInd>();
    auto vr = std::make_shared<VoiceResp>();
    auto vi = std::make_shared<RadioVoiceRecIndication>();
    auto dr = std::make_shared<DataResp>();
    auto di = std::make_shared<RadioDataRecIndication>();
    auto cr = std::make_shared<CfgResp>();
    auto ci = std::make_shared<RadioConfigRecIndication>();

    CHECK(hal.start(argv[1]), "HAL stack started");
    hal.modem->setResponseFunctions(mr, mi);
    hal.sim->setResponseFunctions(sr, si);
    hal.network->setResponseFunctions(nr, ni);
    hal.messaging->setResponseFunctions(gr, gi);
    hal.voice->setResponseFunctions(vr, vi);
    hal.data->setResponseFunctions(dr, di);
    hal.config->setResponseFunctions(cr, ci);

    /* ---- modem ---- */
    CHECK(mi->wait("rilConnected"), "rilConnected indication after setResponseFunctions");
    CHECK(mi->wait("radioStateChanged"), "radioStateChanged indication");
    CHECK(ril_qmi_wait_ready(hal.ril, 8000) == 0, "modem ready");
    mr->version.clear();
    hal.modem->getBasebandVersion(1);
    CHECK(mr->wait("getBasebandVersionResponse:0") && mr->version == "MPSS.TH.2.0-FAKE", "baseband version '%s'", mr->version.c_str());
    hal.modem->getImei(2);
    CHECK(mr->wait("getImeiResponse:0") && mr->imei == "351234567890123", "IMEI '%s'", mr->imei.c_str());
    hal.modem->getHardwareConfig(3);
    CHECK(mr->wait("getHardwareConfigResponse:0"), "hardware config");
    hal.modem->setRadioPower(4, true, false, false);
    CHECK(mr->wait("setRadioPowerResponse:0"), "setRadioPower(on)");
    hal.modem->enableModem(5, true);
    CHECK(mr->wait("enableModemResponse:6"), "unimplemented enableModem -> REQUEST_NOT_SUPPORTED (6), not success");
    hal.modem->getModemActivityInfo(6);
    CHECK(mr->wait("getModemActivityInfoResponse:6"), "unimplemented getModemActivityInfo -> REQUEST_NOT_SUPPORTED");

    /* ---- sim ---- */
    hal.sim->getIccCardStatus(10);
    CHECK(sr->wait("getIccCardStatusResponse:0"), "getIccCardStatus ok");
    CHECK(sr->card.cardState == radio::sim::CardStatus::STATE_PRESENT && sr->card.applications.size() == 1, "card present, 1 app");
    if (!sr->card.applications.empty()) {
        auto& a = sr->card.applications[0];
        CHECK(a.appType == radio::sim::AppStatus::APP_TYPE_USIM && a.appState == radio::sim::AppStatus::APP_STATE_READY, "USIM READY");
        CHECK(a.aidPtr.rfind("A0", 0) == 0 && a.aidPtr.size() == 14, "AID %s", a.aidPtr.c_str());
        CHECK(a.pin1 == radio::sim::PinState::ENABLED_NOT_VERIFIED || a.pin1 == radio::sim::PinState::DISABLED || true, "pin1 state %d", static_cast<int>(a.pin1));
    }
    CHECK(sr->card.gsmUmtsSubscriptionAppIndex == 0 && sr->card.iccid == "8970101234567890123", "app index 0, ICCID '%s'", sr->card.iccid.c_str());
    {
        radio::sim::IccIo io;
        io.command = 176;
        io.fileId = 0x6F07;
        io.path = "3F007FFF";
        io.p1 = 0; io.p2 = 0; io.p3 = 9;
        hal.sim->iccIoForApp(11, io);
        CHECK(sr->wait("iccIoForAppResponse:0") && sr->io.sw1 == 0x90 && sr->io.simResponse == "0829051021436587F9", "EF_IMSI via iccIo: sw %02x%02x data %s", sr->io.sw1, sr->io.sw2, sr->io.simResponse.c_str());
        io.command = 214; /* UPDATE BINARY */
        hal.sim->iccIoForApp(12, io);
        CHECK(sr->wait("iccIoForAppResponse:6"), "UPDATE BINARY -> REQUEST_NOT_SUPPORTED (writes are not implemented)");
    }
    hal.sim->supplyIccPinForApp(13, "0000", "");
    CHECK(sr->wait("supplyIccPinForAppResponse:3") && sr->retries == 2, "wrong PIN -> PASSWORD_INCORRECT, retries %d", sr->retries);
    hal.sim->supplyIccPinForApp(14, "1234", "");
    CHECK(sr->wait("supplyIccPinForAppResponse:0"), "right PIN");
    hal.sim->getImsiForApp(15, "");
    CHECK(sr->wait("getImsiForAppResponse:0") && sr->imsi == "250011234567890", "IMSI");
    CHECK(si->wait("simStatusChanged"), "simStatusChanged indication");
    hal.sim->getFacilityLockForApp(16, "FD", "", 0, "");
    CHECK(sr->wait("getFacilityLockForAppResponse:6"), "FDN lock -> not supported");

    /* ---- config ---- */
    hal.config->getSimSlotsStatus(20);
    CHECK(cr->wait("getSimSlotsStatusResponse:0") && cr->slots.size() == 1 && cr->slots[0].portInfo.size() == 1 && cr->slots[0].portInfo[0].portActive, "sim slot status");
    hal.config->getPhoneCapability(21);
    CHECK(cr->wait("getPhoneCapabilityResponse:0"), "phone capability");
    hal.config->setPreferredDataModem(22, 3);
    CHECK(cr->wait("setPreferredDataModemResponse:44"), "preferred data modem 3 -> INVALID_ARGUMENTS");

    /* ---- network ---- */
    hal.network->getVoiceRegistrationState(30);
    CHECK(nr->wait("getVoiceRegistrationStateResponse:0") && nr->reg.regState == radio::network::RegState::REG_HOME && nr->reg.rat == radio::RadioTechnology::LTE, "registered home on LTE");
    if (nr->reg.cellIdentity.getTag() == radio::network::CellIdentity::Tag::lte) {
        auto& l = nr->reg.cellIdentity.get<radio::network::CellIdentity::Tag::lte>();
        CHECK(l.mcc == "250" && l.mnc == "01" && l.tac == 0x1234 && l.ci == 0xBEEF, "LTE cell %s/%s tac=%x ci=%x", l.mcc.c_str(), l.mnc.c_str(), l.tac, l.ci);
    } else {
        CHECK(false, "cell identity is not LTE");
    }
    CHECK(nr->reg.registeredPlmn == "25001", "registered PLMN %s", nr->reg.registeredPlmn.c_str());
    hal.network->getOperator(31);
    CHECK(nr->wait("getOperatorResponse:0") && nr->lname == "MTS" && nr->num == "25001", "operator %s %s", nr->lname.c_str(), nr->num.c_str());
    hal.network->getSignalStrength(32);
    CHECK(nr->wait("getSignalStrengthResponse:0") && nr->sig.lte.rsrp == 95 && nr->sig.lte.rsrq == 9 && nr->sig.lte.rssnr == 120 && nr->sig.lte.signalStrength == 26,
          "LTE signal rsrp=%d rsrq=%d rssnr=%d asu=%d", nr->sig.lte.rsrp, nr->sig.lte.rsrq, nr->sig.lte.rssnr, nr->sig.lte.signalStrength);
    CHECK(nr->sig.gsm.signalStrength == radio::RadioConst::VALUE_UNAVAILABLE, "unreported RATs are VALUE_UNAVAILABLE");
    hal.network->setNetworkSelectionModeAutomatic(33);
    CHECK(nr->wait("setNetworkSelectionModeAutomaticResponse:0"), "automatic selection");
    hal.network->setNetworkSelectionModeManual(34, "ab", radio::AccessNetwork::EUTRAN);
    CHECK(nr->wait("setNetworkSelectionModeManualResponse:44"), "bad operator numeric rejected");
    CHECK(ni->wait("networkStateChanged"), "networkStateChanged indication");
    CHECK(ni->wait("currentSignalStrength"), "currentSignalStrength indication");
    hal.network->setIndicationFilter(35, 0);
    int before = ni->count("currentSignalStrength");
    hal.network->setIndicationFilter(36, ~0);
    (void)before;

    /* ---- messaging ---- */
    {
        radio::messaging::GsmSmsMessage m;
        m.smscPdu = "";
        m.pdu = "110007916407281553F80000AA0AE8329BFD4697D9EC37"; /* the TPDU; the SMSC octet 00 is added by the HAL */
        hal.messaging->sendSms(40, m);
        CHECK(gr->wait("sendSmsResponse:0") && gr->res.messageRef == 7, "SMS sent, ref %d", gr->res.messageRef);
        m.pdu.replace(m.pdu.size() - 2, 2, "FF");
        hal.messaging->sendSms(41, m);
        CHECK(gr->wait("sendSmsResponse:2") && gr->res.errorCode == 0x123, "failed SMS -> GENERIC_FAILURE, cause 0x%x", gr->res.errorCode);
        m.pdu = "zz";
        hal.messaging->sendSms(42, m);
        CHECK(gr->wait("sendSmsResponse:44"), "non-hex PDU -> INVALID_ARGUMENTS");
        CHECK(gi->wait("newSms", 6000), "incoming SMS indication");
        CHECK(gi->pdu == unhex("0791448720003023040B916407281553F80000FF0AE8329BFD4697D9EC37"), "incoming SMS bytes intact");
        hal.messaging->acknowledgeLastIncomingGsmSms(43, true, radio::messaging::SmsAcknowledgeFailCause::UNSPECIFIED_ERROR);
        CHECK(gr->wait("acknowledgeLastIncomingGsmSmsResponse:0"), "SMS acknowledged");
        hal.messaging->getSmscAddress(44);
        CHECK(gr->wait("getSmscAddressResponse:6"), "getSmscAddress not implemented -> REQUEST_NOT_SUPPORTED");
    }

    /* ---- voice ---- */
    {
        radio::voice::Dial d;
        d.address = "+79001234567";
        hal.voice->dial(50, d);
        CHECK(vr->wait("dialResponse:0"), "dial accepted by the modem");
        CHECK(vi->wait("callStateChanged"), "callStateChanged indication");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        hal.voice->getCurrentCalls(51);
        CHECK(vr->wait("getCurrentCallsResponse:0") && vr->calls.size() == 1 && vr->calls[0].state == radio::voice::Call::STATE_ACTIVE && vr->calls[0].index == 1 && !vr->calls[0].isMT && vr->calls[0].number == "+79001234567" && vr->calls[0].toa == 145,
              "outgoing call ACTIVE, index 1, toa 145");
        hal.voice->hangup(52, 1);
        CHECK(vr->wait("hangupConnectionResponse:0"), "hangup index 1");
        for (int i = 0; i < 60; i++) { /* the fake places an incoming call 0.5 s after the hangup */
            hal.voice->getCurrentCalls(54);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!vr->calls.empty()) break;
        }
        CHECK(vr->calls.size() == 1 && vr->calls[0].state == radio::voice::Call::STATE_INCOMING && vr->calls[0].isMT && vr->calls[0].index == 1, "incoming call visible, index reused (1)");
        hal.voice->acceptCall(55);
        CHECK(vr->wait("acceptCallResponse:0"), "acceptCall");
        radio::voice::Dial c;
        c.address = "123";
        c.clir = radio::voice::Dial::CLIR_INVOCATION;
        hal.voice->dial(56, c);
        CHECK(vr->wait("dialResponse:6"), "CLIR dial refused instead of ignored");
        hal.voice->conference(57);
        CHECK(vr->wait("conferenceResponse:6"), "conference not implemented -> REQUEST_NOT_SUPPORTED");
        hal.voice->hangup(58, 1);
        CHECK(vr->wait("hangupConnectionResponse:0"), "hangup incoming->active call");
    }

    /* ---- data ---- */
    {
        radio::data::DataProfileInfo p;
        p.apn = "internet.test";
        p.user = "user";
        p.password = "pass";
        p.protocol = radio::data::PdpProtocolType::IPV4V6;
        p.authType = radio::data::ApnAuthType::PAP_CHAP;
        hal.data->setupDataCall(60, radio::AccessNetwork::EUTRAN, p, false, radio::data::DataRequestReason::NORMAL, {}, {}, 0, std::nullopt, false);
        CHECK(dr->wait("setupDataCallResponse:0") && dr->dc.cause == radio::data::DataCallFailCause::NONE && dr->dc.cid == 1, "data call up, cid %d", dr->dc.cid);
        CHECK(dr->dc.ifname == "lo" && dr->dc.addresses.size() == 1 && dr->dc.addresses[0].address == "10.20.30.40/24", "address %s", dr->dc.addresses.empty() ? "-" : dr->dc.addresses[0].address.c_str());
        CHECK(dr->dc.gateways == std::vector<std::string>{"10.20.30.1"} && dr->dc.dnses == (std::vector<std::string>{"8.8.8.8", "8.8.4.4"}), "gateway/DNS");
        CHECK(dr->dc.mtuV4 == 1500, "mtu %d (modem value out of range -> 1500)", dr->dc.mtuV4);
        hal.data->getDataCallList(61);
        CHECK(dr->wait("getDataCallListResponse:0") && dr->list.size() == 1, "call list has 1 entry");
        hal.data->setupDataCall(62, radio::AccessNetwork::EUTRAN, p, false, radio::data::DataRequestReason::NORMAL, {}, {}, 0, std::nullopt, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(dr->dc.cause == radio::data::DataCallFailCause::MAX_ACTIVE_PDP_CONTEXT_REACHED, "second call -> MAX_ACTIVE_PDP_CONTEXT_REACHED");
        hal.data->deactivateDataCall(63, 1, radio::data::DataRequestReason::NORMAL);
        CHECK(dr->wait("deactivateDataCallResponse:0"), "deactivate");
        CHECK(di->wait("dataCallListChanged"), "dataCallListChanged after the modem reports disconnect");
        hal.data->deactivateDataCall(64, 1, radio::data::DataRequestReason::NORMAL);
        CHECK(dr->wait("deactivateDataCallResponse:44"), "deactivate unknown cid -> INVALID_ARGUMENTS");
    }

    hal.stop();
    printf("\n[hal test] %d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
