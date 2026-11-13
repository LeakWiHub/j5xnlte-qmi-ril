// SPDX-License-Identifier: Apache-2.0
// IRadioNetwork, IRadioMessaging, IRadioVoice, IRadioData.
#define LOG_TAG "j5xnlte-ril"
#include <android/log.h>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <aidl/android/hardware/radio/RadioAccessFamily.h>
#include <aidl/android/hardware/radio/RadioConst.h>
#include <aidl/android/hardware/radio/network/IndicationFilter.h>

#include "radio_impl.h"

namespace j5ril {

using radio::RadioConst;
using radio::RadioError;
using radio::RadioIndicationType;
using radio::RadioTechnology;

#define REQUIRE_RESP(r) \
    auto r = mResp;     \
    if (!r) return ::ndk::ScopedAStatus::ok()
#define REPLY(call) \
    {               \
        (void)(call); \
        return;     \
    }

/* ================================================================== IRadioNetwork */
static RadioTechnology ratToTech(int rat) {
    switch (rat) {
        case RIL_RAT_GSM: return RadioTechnology::GSM;
        case RIL_RAT_UMTS: return RadioTechnology::UMTS;
        case RIL_RAT_LTE: return RadioTechnology::LTE;
        default: return RadioTechnology::UNKNOWN;
    }
}

static int asu(int dbm) { return std::clamp((dbm + 113) / 2, 0, 31); }

radio::network::RegStateResult RadioNetworkImpl::regState(const ril_serving_t& s, bool dataDomain) {
    using namespace radio::network;
    RegStateResult r;
    int attach = dataDomain ? s.ps_attach : s.cs_attach;
    switch (s.reg_state) {
        case 1: r.regState = attach == 1 ? (s.roaming == 1 ? RegState::REG_ROAMING : RegState::REG_HOME) : RegState::NOT_REG_MT_NOT_SEARCHING_OP; break;
        case 2: r.regState = RegState::NOT_REG_MT_SEARCHING_OP; break;
        case 3: r.regState = RegState::REG_DENIED; break;
        case 0: r.regState = RegState::NOT_REG_MT_NOT_SEARCHING_OP; break;
        default: r.regState = RegState::UNKNOWN;
    }
    bool registered = r.regState == RegState::REG_HOME || r.regState == RegState::REG_ROAMING;
    if (!registered) return r;
    r.rat = ratToTech(s.rat);
    std::string mcc, mnc;
    if (s.plmn_valid) {
        std::string p = plmnString(s.mcc, s.mnc);
        mcc = p.substr(0, 3);
        mnc = p.substr(3);
        r.registeredPlmn = p;
    }
    int lac = s.lac_valid ? s.lac : RadioConst::VALUE_UNAVAILABLE;
    int cid = s.cid_valid ? static_cast<int>(s.cid) : RadioConst::VALUE_UNAVAILABLE;
    if (s.rat == RIL_RAT_LTE) {
        CellIdentityLte c;
        c.mcc = mcc;
        c.mnc = mnc;
        c.ci = cid;
        c.pci = RadioConst::VALUE_UNAVAILABLE;
        c.tac = lac;
        c.earfcn = RadioConst::VALUE_UNAVAILABLE;
        c.bandwidth = RadioConst::VALUE_UNAVAILABLE;
        r.cellIdentity = CellIdentity::make<CellIdentity::Tag::lte>(c);
    } else if (s.rat == RIL_RAT_UMTS) {
        CellIdentityWcdma c;
        c.mcc = mcc;
        c.mnc = mnc;
        c.lac = lac;
        c.cid = cid;
        c.psc = RadioConst::VALUE_UNAVAILABLE;
        c.uarfcn = RadioConst::VALUE_UNAVAILABLE;
        r.cellIdentity = CellIdentity::make<CellIdentity::Tag::wcdma>(c);
    } else if (s.rat == RIL_RAT_GSM) {
        CellIdentityGsm c;
        c.mcc = mcc;
        c.mnc = mnc;
        c.lac = lac;
        c.cid = cid;
        c.arfcn = RadioConst::VALUE_UNAVAILABLE;
        c.bsic = RadioConst::VALUE_UNAVAILABLE_BYTE;
        r.cellIdentity = CellIdentity::make<CellIdentity::Tag::gsm>(c);
    }
    return r;
}

::ndk::ScopedAStatus RadioNetworkImpl::getVoiceRegistrationState(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        ril_serving_t s;
        int rc = h_.ready() ? ril_net_serving(h_.ril, &s) : -ENOTCONN;
        if (rc) REPLY(resp->getVoiceRegistrationStateResponse(rspInfo(serial, mapError(rc)), {}));
        resp->getVoiceRegistrationStateResponse(rspInfo(serial), regState(s, false));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::getDataRegistrationState(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        ril_serving_t s;
        int rc = h_.ready() ? ril_net_serving(h_.ril, &s) : -ENOTCONN;
        if (rc) REPLY(resp->getDataRegistrationStateResponse(rspInfo(serial, mapError(rc)), {}));
        resp->getDataRegistrationStateResponse(rspInfo(serial), regState(s, true));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::getOperator(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        ril_serving_t s;
        int rc = h_.ready() ? ril_net_serving(h_.ril, &s) : -ENOTCONN;
        if (rc) REPLY(resp->getOperatorResponse(rspInfo(serial, mapError(rc)), "", "", ""));
        if (!s.plmn_valid) REPLY(resp->getOperatorResponse(rspInfo(serial), "", "", "")); /* not registered: no operator */
        std::string num = plmnString(s.mcc, s.mnc);
        std::string name = s.operator_name[0] ? s.operator_name : num;
        resp->getOperatorResponse(rspInfo(serial), name, name, num);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::getSignalStrength(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        radio::network::SignalStrength ss;
        ril_signal_t s;
        int rc = h_.ready() ? ril_net_signal(h_.ril, &s) : -ENOTCONN;
        if (rc) REPLY(resp->getSignalStrengthResponse(rspInfo(serial, mapError(rc)), ss));
        resp->getSignalStrengthResponse(rspInfo(serial), signalFrom(s));
    });
    return ::ndk::ScopedAStatus::ok();
}

radio::network::SignalStrength RadioNetworkImpl::signalFrom(const ril_signal_t& s) {
    radio::network::SignalStrength ss;
    const int U = RadioConst::VALUE_UNAVAILABLE;
    /* every sub-structure starts "unavailable": the framework treats INT_MAX as "not reported" */
    ss.gsm.signalStrength = ss.gsm.bitErrorRate = ss.gsm.timingAdvance = U;
    ss.wcdma.signalStrength = ss.wcdma.bitErrorRate = ss.wcdma.rscp = ss.wcdma.ecno = U;
    ss.lte.signalStrength = ss.lte.rsrp = ss.lte.rsrq = ss.lte.rssnr = ss.lte.cqi = ss.lte.timingAdvance = ss.lte.cqiTableIndex = U;
    if (s.gsm_valid) ss.gsm.signalStrength = asu(s.gsm_rssi);
    if (s.wcdma_valid) ss.wcdma.signalStrength = asu(s.wcdma_rssi);
    if (s.lte_valid) {
        ss.lte.signalStrength = asu(s.lte_rssi);
        ss.lte.rsrp = -s.lte_rsrp; /* QMI dBm (negative) -> AIDL "dBm * -1" */
        ss.lte.rsrq = -s.lte_rsrq;
        ss.lte.rssnr = s.lte_snr; /* 0.1 dB units in both */
    }
    if (s.generic_valid) { /* NAS 0x20 fallback: one RSSI value for the current RAT */
        if (s.generic_rat == RIL_RAT_GSM) ss.gsm.signalStrength = asu(s.generic_dbm);
        else if (s.generic_rat == RIL_RAT_UMTS) ss.wcdma.signalStrength = asu(s.generic_dbm);
        else if (s.generic_rat == RIL_RAT_LTE) ss.lte.signalStrength = asu(s.generic_dbm);
    }
    return ss;
}

::ndk::ScopedAStatus RadioNetworkImpl::getVoiceRadioTechnology(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        ril_serving_t s;
        int rc = h_.ready() ? ril_net_serving(h_.ril, &s) : -ENOTCONN;
        resp->getVoiceRadioTechnologyResponse(rspInfo(serial, mapError(rc)), rc ? RadioTechnology::UNKNOWN : ratToTech(s.rat));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::getNetworkSelectionMode(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        int manual = 0, rc = h_.ready() ? ril_net_get_selection_mode(h_.ril, &manual) : -ENOTCONN;
        resp->getNetworkSelectionModeResponse(rspInfo(serial, mapError(rc)), manual != 0);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::setNetworkSelectionModeAutomatic(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        int rc = h_.ready() ? ril_net_register_auto(h_.ril) : -ENOTCONN;
        resp->setNetworkSelectionModeAutomaticResponse(rspInfo(serial, mapError(rc)));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::setNetworkSelectionModeManual(int32_t serial, const std::string& numeric, radio::AccessNetwork ran) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, numeric, ran] {
        if ((numeric.size() != 5 && numeric.size() != 6) ||
            !std::all_of(numeric.begin(), numeric.end(), [](char c) { return c >= '0' && c <= '9'; }))
            REPLY(resp->setNetworkSelectionModeManualResponse(rspInfo(serial, RadioError::INVALID_ARGUMENTS)));
        int mcc = std::atoi(numeric.substr(0, 3).c_str()), mnc = std::atoi(numeric.substr(3).c_str());
        int rat = ran == radio::AccessNetwork::GERAN ? RIL_RAT_GSM : ran == radio::AccessNetwork::UTRAN ? RIL_RAT_UMTS
                  : ran == radio::AccessNetwork::EUTRAN ? RIL_RAT_LTE : RIL_RAT_UMTS;
        if (ran == radio::AccessNetwork::UNKNOWN && h_.ready()) {
            ril_serving_t s;
            if (ril_net_serving(h_.ril, &s) == 0 && s.rat) rat = s.rat;
        }
        int rc = h_.ready() ? ril_net_register_manual(h_.ril, mcc, mnc, rat) : -ENOTCONN;
        resp->setNetworkSelectionModeManualResponse(rspInfo(serial, mapError(rc)));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::getAvailableNetworks(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] { /* may block for minutes: it occupies one executor thread, the others keep serving */
        std::vector<ril_scan_entry_t> list(64);
        size_t n = 0;
        int rc = h_.ready() ? ril_net_scan(h_.ril, list.data(), list.size(), &n) : -ENOTCONN;
        std::vector<radio::network::OperatorInfo> out;
        for (size_t i = 0; rc == 0 && i < n; i++) {
            radio::network::OperatorInfo o;
            o.operatorNumeric = plmnString(list[i].mcc, list[i].mnc);
            o.alphaLong = o.alphaShort = list[i].name[0] ? list[i].name : o.operatorNumeric;
            int forbidden = (list[i].status >> 4) & 3, inuse = list[i].status & 3;
            o.status = forbidden == 1 ? static_cast<int32_t>(radio::network::OperatorInfo::STATUS_FORBIDDEN)
                       : inuse == 1   ? static_cast<int32_t>(radio::network::OperatorInfo::STATUS_CURRENT)
                       : inuse == 2   ? static_cast<int32_t>(radio::network::OperatorInfo::STATUS_AVAILABLE)
                                      : static_cast<int32_t>(radio::network::OperatorInfo::STATUS_UNKNOWN);
            out.push_back(o);
        }
        resp->getAvailableNetworksResponse(rspInfo(serial, mapError(rc)), out);
    });
    return ::ndk::ScopedAStatus::ok();
}

using radio::RadioAccessFamily;
static constexpr int fam(RadioAccessFamily f) { return static_cast<int>(f); }
static const int kGsmBits = fam(RadioAccessFamily::GPRS) | fam(RadioAccessFamily::EDGE) | fam(RadioAccessFamily::GSM);
static const int kUmtsBits = fam(RadioAccessFamily::UMTS) | fam(RadioAccessFamily::HSDPA) | fam(RadioAccessFamily::HSUPA) |
                             fam(RadioAccessFamily::HSPA) | fam(RadioAccessFamily::HSPAP);
static const int kLteBits = fam(RadioAccessFamily::LTE) | fam(RadioAccessFamily::LTE_CA);

::ndk::ScopedAStatus RadioNetworkImpl::setAllowedNetworkTypesBitmap(int32_t serial, int32_t bitmap) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, bitmap] {
        unsigned mask = 0;
        if (bitmap & kGsmBits) mask |= RIL_RATMASK_GSM;
        if (bitmap & kUmtsBits) mask |= RIL_RATMASK_UMTS;
        if (bitmap & kLteBits) mask |= RIL_RATMASK_LTE;
        if (!mask) REPLY(resp->setAllowedNetworkTypesBitmapResponse(rspInfo(serial, RadioError::MODE_NOT_SUPPORTED))); /* only 2G/3G/4G exist on this modem */
        int rc = h_.ready() ? ril_net_set_mode_pref(h_.ril, mask) : -ENOTCONN;
        resp->setAllowedNetworkTypesBitmapResponse(rspInfo(serial, mapError(rc)));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::getAllowedNetworkTypesBitmap(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        unsigned mask = 0;
        int rc = h_.ready() ? ril_net_get_mode_pref(h_.ril, &mask) : -ENOTCONN;
        int bm = 0;
        if (mask & RIL_RATMASK_GSM) bm |= kGsmBits;
        if (mask & RIL_RATMASK_UMTS) bm |= kUmtsBits;
        if (mask & RIL_RATMASK_LTE) bm |= kLteBits;
        resp->getAllowedNetworkTypesBitmapResponse(rspInfo(serial, mapError(rc)), rc ? 0 : bm);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioNetworkImpl::setIndicationFilter(int32_t serial, int32_t filter) {
    REQUIRE_RESP(resp);
    {
        std::lock_guard<std::mutex> l(h_.mu);
        h_.indicationFilter = filter; /* enforced in notifyNetworkChanged / notifySignalChanged */
    }
    resp->setIndicationFilterResponse(rspInfo(serial));
    return ::ndk::ScopedAStatus::ok();
}

/* The modem is not asked to apply the thresholds: this HAL reports every change instead (a superset of what
 * the framework asked for, which the HAL contract allows). Not an ack-and-ignore: the behaviour is documented. */
::ndk::ScopedAStatus RadioNetworkImpl::setSignalStrengthReportingCriteria(int32_t serial,
                                                                          const std::vector<radio::network::SignalThresholdInfo>&) {
    REQUIRE_RESP(resp);
    resp->setSignalStrengthReportingCriteriaResponse(rspInfo(serial));
    return ::ndk::ScopedAStatus::ok();
}

void RadioNetworkImpl::notifyNetworkChanged() {
    /* IndicationFilter::FULL_NETWORK_STATE only asks for fewer *detail* updates; registration changes always go out. */
    auto ind = mInd;
    if (ind) ind->networkStateChanged(RadioIndicationType::UNSOLICITED);
}

void RadioNetworkImpl::notifySignalChanged() {
    auto ind = mInd;
    int32_t f;
    {
        std::lock_guard<std::mutex> l(h_.mu);
        f = h_.indicationFilter;
    }
    if (!ind || !(f & static_cast<int32_t>(radio::network::IndicationFilter::SIGNAL_STRENGTH))) return;
    h_.exec.post([this, ind] {
        ril_signal_t s;
        if (h_.ready() && ril_net_signal(h_.ril, &s) == 0) ind->currentSignalStrength(RadioIndicationType::UNSOLICITED, signalFrom(s));
    });
}

/* ================================================================== IRadioMessaging */
void RadioMessagingImpl::notifyNewSms(const std::vector<uint8_t>& pdu) {
    auto ind = mInd;
    if (ind) ind->newSms(RadioIndicationType::UNSOLICITED, pdu);
}

/* Android hands the SMSC (hex, with its length octet; empty = default) and the TPDU separately; QMI wants both
 * in one buffer. An empty SMSC becomes a zero length octet (= use the modem/SIM default). */
void RadioMessagingImpl::send(int32_t serial, const radio::messaging::GsmSmsMessage& m, bool expectMore) {
    auto resp = mResp;
    if (!resp) return;
    h_.exec.post([this, resp, serial, m, expectMore] {
        radio::messaging::SendSmsResult res;
        res.errorCode = -1;
        auto reply = [&](RadioError e) {
            auto info = rspInfo(serial, e);
            if (expectMore) (void)resp->sendSmsExpectMoreResponse(info, res);
            else (void)resp->sendSmsResponse(info, res);
        };
        std::vector<uint8_t> smsc, tpdu, all;
        if (!fromHex(m.smscPdu, &smsc) || !fromHex(m.pdu, &tpdu) || tpdu.empty()) return reply(RadioError::INVALID_ARGUMENTS);
        if (smsc.empty()) smsc.push_back(0);
        all = smsc;
        all.insert(all.end(), tpdu.begin(), tpdu.end());
        if (!h_.ready()) return reply(RadioError::RADIO_NOT_AVAILABLE);
        int ref = 0, cls = 0, cause = 0;
        int rc = ril_sms_send(h_.ril, all.data(), all.size(), &ref, &cls, &cause);
        res.messageRef = ref;
        if (rc == 0) return reply(RadioError::NONE);
        if (cause) res.errorCode = cause;
        /* error class per QMI WMS: 0 temporary (worth a retry), 1 permanent */
        reply(QMI_E_IS_QMI(rc) && cls == 0 ? RadioError::SMS_SEND_FAIL_RETRY : mapError(rc));
    });
}

::ndk::ScopedAStatus RadioMessagingImpl::sendSms(int32_t serial, const radio::messaging::GsmSmsMessage& m) {
    if (mResp) send(serial, m, false);
    return ::ndk::ScopedAStatus::ok();
}
::ndk::ScopedAStatus RadioMessagingImpl::sendSmsExpectMore(int32_t serial, const radio::messaging::GsmSmsMessage& m) {
    if (mResp) send(serial, m, true);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioMessagingImpl::acknowledgeLastIncomingGsmSms(int32_t serial, bool success,
                                                                       radio::messaging::SmsAcknowledgeFailCause cause) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, success, cause] {
        int rc = h_.ready() ? ril_sms_ack(h_.ril, success, static_cast<int>(cause)) : -ENOTCONN;
        resp->acknowledgeLastIncomingGsmSmsResponse(rspInfo(serial, mapError(rc)));
    });
    return ::ndk::ScopedAStatus::ok();
}

/* The modem builds the RP-ACK itself; a caller supplied ACK PDU body is not forwarded. */
::ndk::ScopedAStatus RadioMessagingImpl::acknowledgeIncomingGsmSmsWithPdu(int32_t serial, bool success, const std::string&) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, success] {
        int rc = h_.ready() ? ril_sms_ack(h_.ril, success, static_cast<int>(radio::messaging::SmsAcknowledgeFailCause::UNSPECIFIED_ERROR)) : -ENOTCONN;
        resp->acknowledgeIncomingGsmSmsWithPduResponse(rspInfo(serial, mapError(rc)));
    });
    return ::ndk::ScopedAStatus::ok();
}

/* ================================================================== IRadioVoice */
/* The voice DSP path (CS-Voice) is not opened by Android's generic audio HAL, so the module's j5x-voice daemon does it.
 * It follows this one-word file: "active" while a call is dialing/alerting/active, otherwise "idle". */
static void publishVoiceState(bool active) {
    static int last = -1;
    if (last == (active ? 1 : 0)) return;
    last = active ? 1 : 0;
    const char* dir = "/data/local/tmp/j5ril-state";
    mkdir(dir, 0755);
    std::string tmp = std::string(dir) + "/.call_state.tmp", dst = std::string(dir) + "/call_state";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fputs(active ? "active\n" : "idle\n", f);
    fclose(f);
    rename(tmp.c_str(), dst.c_str());
}

void RadioVoiceImpl::notifyCallsChanged() {
    auto ind = mInd;
    if (ind) ind->callStateChanged(RadioIndicationType::UNSOLICITED);
    h_.exec.post([this] {
        Snapshot s;
        radio::RadioError err;
        if (!snapshot(&s, &err)) return;
        bool active = false;
        for (auto& c : s.calls)
            active |= c.state == RIL_CALL_ACTIVE || c.state == RIL_CALL_DIALING || c.state == RIL_CALL_ALERTING;
        publishVoiceState(active);
    });
}

/* Android indexes calls 1..7 and keeps them stable for the life of a call; QMI call ids are the modem's own. */
bool RadioVoiceImpl::snapshot(Snapshot* out, RadioError* err) {
    *err = RadioError::NONE;
    if (!h_.ready()) {
        *err = RadioError::RADIO_NOT_AVAILABLE;
        return false;
    }
    ril_call_t cl[RIL_MAX_CALLS];
    size_t n = 0;
    int rc = ril_voice_calls(h_.ril, cl, RIL_MAX_CALLS, &n);
    if (rc) {
        *err = mapError(rc);
        return false;
    }
    std::lock_guard<std::mutex> l(mu_);
    for (auto it = idToIndex_.begin(); it != idToIndex_.end();) {
        bool alive = false;
        for (size_t i = 0; i < n; i++) alive |= cl[i].id == it->first;
        it = alive ? std::next(it) : idToIndex_.erase(it);
    }
    out->calls.assign(cl, cl + n);
    out->index.clear();
    for (size_t i = 0; i < n; i++) {
        auto f = idToIndex_.find(cl[i].id);
        if (f == idToIndex_.end()) {
            int idx = 1;
            for (;; idx++) {
                bool used = false;
                for (auto& kv : idToIndex_) used |= kv.second == idx;
                if (!used) break;
            }
            f = idToIndex_.emplace(cl[i].id, idx).first;
        }
        out->index.push_back(f->second);
    }
    return true;
}

int RadioVoiceImpl::idForIndex(int index) {
    std::lock_guard<std::mutex> l(mu_);
    for (auto& kv : idToIndex_)
        if (kv.second == index) return kv.first;
    return -1;
}

int RadioVoiceImpl::activeId() {
    Snapshot s;
    RadioError e;
    if (!snapshot(&s, &e)) return -1;
    for (auto& c : s.calls)
        if (c.state == RIL_CALL_ACTIVE) return c.id;
    return -1;
}

::ndk::ScopedAStatus RadioVoiceImpl::getCurrentCalls(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        Snapshot s;
        RadioError e;
        std::vector<radio::voice::Call> out;
        if (snapshot(&s, &e)) {
            for (size_t i = 0; i < s.calls.size(); i++) {
                radio::voice::Call c;
                c.state = s.calls[i].state;
                c.index = s.index[i];
                c.toa = s.calls[i].number[0] == '+' ? 145 : 129;
                c.isMpty = s.calls[i].multiparty;
                c.isMT = s.calls[i].incoming;
                c.als = 0;
                c.isVoice = true;
                c.isVoicePrivacy = false;
                c.number = s.calls[i].number;
                c.numberPresentation = s.calls[i].number[0] ? static_cast<int32_t>(radio::voice::Call::PRESENTATION_ALLOWED) : static_cast<int32_t>(radio::voice::Call::PRESENTATION_UNKNOWN);
                c.namePresentation = radio::voice::Call::PRESENTATION_UNKNOWN;
                out.push_back(c);
            }
        }
        resp->getCurrentCallsResponse(rspInfo(serial, e), out);
    });
    return ::ndk::ScopedAStatus::ok();
}

void RadioVoiceImpl::dialCommon(int32_t serial, const std::string& number, bool emergency) {
    auto resp = mResp;
    if (!resp) return;
    h_.exec.post([this, resp, serial, number, emergency] {
        int id = -1, rc = h_.ready() ? ril_voice_dial(h_.ril, number.c_str(), &id) : -ENOTCONN;
        auto info = rspInfo(serial, mapError(rc));
        if (emergency) (void)resp->emergencyDialResponse(info);
        else (void)resp->dialResponse(info);
    });
}

::ndk::ScopedAStatus RadioVoiceImpl::dial(int32_t serial, const radio::voice::Dial& d) {
    REQUIRE_RESP(resp);
    if (d.clir != radio::voice::Dial::CLIR_DEFAULT) { /* no CLIR control in this backend: refuse instead of ignoring */
        (void)resp->dialResponse(rspInfo(serial, RadioError::REQUEST_NOT_SUPPORTED));
        return ::ndk::ScopedAStatus::ok();
    }
    dialCommon(serial, d.address, false);
    return ::ndk::ScopedAStatus::ok();
}

/* Emergency call routing/categories are left to the modem; calls without a SIM are NOT supported by this RIL. */
::ndk::ScopedAStatus RadioVoiceImpl::emergencyDial(int32_t serial, const radio::voice::Dial& d, int32_t, const std::vector<std::string>&,
                                                   radio::voice::EmergencyCallRouting, bool, bool) {
    REQUIRE_RESP(resp);
    (void)resp;
    dialCommon(serial, d.address, true);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoiceImpl::acceptCall(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        Snapshot s;
        RadioError e;
        int id = -1;
        if (snapshot(&s, &e)) {
            for (auto& c : s.calls)
                if (c.state == RIL_CALL_INCOMING || c.state == RIL_CALL_WAITING) { id = c.id; break; }
            if (id < 0) e = RadioError::INVALID_CALL_ID;
        }
        if (e == RadioError::NONE) e = mapError(ril_voice_answer(h_.ril, id));
        resp->acceptCallResponse(rspInfo(serial, e));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoiceImpl::rejectCall(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        Snapshot s;
        RadioError e;
        int id = -1;
        if (snapshot(&s, &e)) {
            for (auto& c : s.calls)
                if (c.state == RIL_CALL_INCOMING || c.state == RIL_CALL_WAITING) { id = c.id; break; }
            if (id < 0) e = RadioError::INVALID_CALL_ID;
        }
        if (e == RadioError::NONE) e = mapError(ril_voice_hangup(h_.ril, id)); /* END_CALL on an incoming call rejects it */
        resp->rejectCallResponse(rspInfo(serial, e));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoiceImpl::hangup(int32_t serial, int32_t index) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, index] {
        Snapshot s;
        RadioError e;
        (void)snapshot(&s, &e); /* refreshes the index map */
        int id = idForIndex(index);
        if (id < 0) e = e == RadioError::NONE ? RadioError::INVALID_CALL_ID : e;
        else e = mapError(ril_voice_hangup(h_.ril, id));
        resp->hangupConnectionResponse(rspInfo(serial, e));
    });
    return ::ndk::ScopedAStatus::ok();
}

/* Waiting and held calls are ended one by one with END_CALL; no supplementary-service (SS) requests are involved. */
::ndk::ScopedAStatus RadioVoiceImpl::hangupWaitingOrBackground(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        Snapshot s;
        RadioError e;
        if (snapshot(&s, &e)) {
            for (auto& c : s.calls) {
                if (c.state != RIL_CALL_WAITING && c.state != RIL_CALL_HOLDING) continue;
                RadioError r = mapError(ril_voice_hangup(h_.ril, c.id));
                if (r != RadioError::NONE) e = r;
            }
        }
        resp->hangupWaitingOrBackgroundResponse(rspInfo(serial, e));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoiceImpl::startDtmf(int32_t serial, const std::string& s) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, s] {
        int id = activeId();
        RadioError e = s.size() != 1 ? RadioError::INVALID_ARGUMENTS : id < 0 ? RadioError::INVALID_CALL_ID : mapError(ril_voice_dtmf(h_.ril, id, s[0], 1));
        resp->startDtmfResponse(rspInfo(serial, e));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoiceImpl::stopDtmf(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        int id = activeId();
        RadioError e = id < 0 ? RadioError::INVALID_CALL_ID : mapError(ril_voice_dtmf(h_.ril, id, '0', 0));
        resp->stopDtmfResponse(rspInfo(serial, e));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioVoiceImpl::sendDtmf(int32_t serial, const std::string& s) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, s] {
        int id = activeId();
        RadioError e = id < 0 ? RadioError::INVALID_CALL_ID : RadioError::NONE;
        for (size_t i = 0; e == RadioError::NONE && i < s.size(); i++) {
            e = mapError(ril_voice_dtmf(h_.ril, id, s[i], 1));
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            RadioError st = mapError(ril_voice_dtmf(h_.ril, id, s[i], 0));
            if (e == RadioError::NONE) e = st;
        }
        resp->sendDtmfResponse(rspInfo(serial, e));
    });
    return ::ndk::ScopedAStatus::ok();
}

/* The QMI end-of-call reason is not decoded by this backend: the true answer is "unspecified". */
::ndk::ScopedAStatus RadioVoiceImpl::getLastCallFailCause(int32_t serial) {
    REQUIRE_RESP(resp);
    radio::voice::LastCallFailCauseInfo i;
    i.causeCode = radio::voice::LastCallFailCause::ERROR_UNSPECIFIED;
    resp->getLastCallFailCauseResponse(rspInfo(serial), i);
    return ::ndk::ScopedAStatus::ok();
}

/* ================================================================== IRadioData */
static radio::data::SetupDataCallResult resultFrom(const ril_data_call_t& d, int cid) {
    radio::data::SetupDataCallResult r;
    r.cause = radio::data::DataCallFailCause::NONE;
    r.suggestedRetryTime = -1;
    r.cid = cid;
    r.active = radio::data::SetupDataCallResult::DATA_CONNECTION_STATUS_ACTIVE;
    r.type = radio::data::PdpProtocolType::IP;
    r.ifname = d.ifname;
    r.mtuV4 = d.mtu > 0 && d.mtu <= 65535 ? d.mtu : 1500;
    auto ip = [](const uint8_t* b) {
        char s[20];
        snprintf(s, sizeof(s), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return std::string(s);
    };
    int prefix = 0;
    for (int i = 0; i < 4; i++)
        for (int b = 7; b >= 0; b--) prefix += (d.mask[i] >> b) & 1;
    radio::data::LinkAddress la;
    la.address = ip(d.ip) + "/" + std::to_string(prefix ? prefix : 32);
    la.addressProperties = radio::data::LinkAddress::ADDRESS_PROPERTY_NONE;
    la.deprecationTime = RadioConst::VALUE_UNAVAILABLE_LONG;
    la.expirationTime = RadioConst::VALUE_UNAVAILABLE_LONG;
    r.addresses.push_back(la);
    if (d.has_gw) r.gateways.push_back(ip(d.gw));
    if (d.has_dns1) r.dnses.push_back(ip(d.dns1));
    if (d.has_dns2) r.dnses.push_back(ip(d.dns2));
    return r;
}

static constexpr int kCid = 1; /* this backend runs at most one data call */

::ndk::ScopedAStatus RadioDataImpl::setupDataCall(int32_t serial, radio::AccessNetwork, const radio::data::DataProfileInfo& p, bool,
                                                  radio::data::DataRequestReason, const std::vector<radio::data::LinkAddress>&,
                                                  const std::vector<std::string>&, int32_t, const std::optional<radio::data::SliceInfo>&, bool) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, p] {
        /* HAL contract: NONE for both outcomes, the real result is SetupDataCallResult.cause */
        auto fail = [&](radio::data::DataCallFailCause c, RadioError e = RadioError::NONE) {
            radio::data::SetupDataCallResult r;
            r.cause = c;
            r.suggestedRetryTime = -1;
            r.cid = -1;
            r.active = radio::data::SetupDataCallResult::DATA_CONNECTION_STATUS_INACTIVE;
            (void)resp->setupDataCallResponse(rspInfo(serial, e), r);
        };
        if (!h_.ready()) return fail(radio::data::DataCallFailCause::ERROR_UNSPECIFIED, RadioError::RADIO_NOT_AVAILABLE);
        {
            std::lock_guard<std::mutex> l(h_.mu);
            if (!h_.dataAllowed) return fail(radio::data::DataCallFailCause::ERROR_UNSPECIFIED, RadioError::OP_NOT_ALLOWED_BEFORE_REG_TO_NW);
            if (h_.dataCall.active) return fail(radio::data::DataCallFailCause::MAX_ACTIVE_PDP_CONTEXT_REACHED);
        }
        if (p.protocol == radio::data::PdpProtocolType::IPV6) return fail(radio::data::DataCallFailCause::ONLY_IPV4_ALLOWED);
        ril_data_req_t rq;
        memset(&rq, 0, sizeof(rq));
        snprintf(rq.apn, sizeof(rq.apn), "%s", p.apn.c_str());
        snprintf(rq.user, sizeof(rq.user), "%s", p.user.c_str());
        snprintf(rq.pass, sizeof(rq.pass), "%s", p.password.c_str());
        rq.auth = static_cast<int>(p.authType);
        rq.ip_family = 4;
        ril_data_call_t dc;
        int rc = ril_data_start(h_.ril, &rq, &dc);
        if (rc) {
            __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "data call failed: %d", rc);
            return fail(radio::data::DataCallFailCause::ERROR_UNSPECIFIED, rc == -EINVAL ? RadioError::INVALID_ARGUMENTS : RadioError::NONE);
        }
        {
            std::lock_guard<std::mutex> l(h_.mu);
            h_.dataCall.active = true;
            h_.dataCall.dc = dc;
        }
        (void)resp->setupDataCallResponse(rspInfo(serial), resultFrom(dc, kCid));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioDataImpl::deactivateDataCall(int32_t serial, int32_t cid, radio::data::DataRequestReason) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, cid] {
        ril_data_call_t dc;
        {
            std::lock_guard<std::mutex> l(h_.mu);
            if (!h_.dataCall.active || cid != kCid) REPLY(resp->deactivateDataCallResponse(rspInfo(serial, RadioError::INVALID_ARGUMENTS)));
            dc = h_.dataCall.dc;
            h_.dataCall.active = false;
        }
        int rc = h_.ready() ? ril_data_stop(h_.ril, dc.handle, dc.ifname) : -ENOTCONN;
        resp->deactivateDataCallResponse(rspInfo(serial, mapError(rc)));
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioDataImpl::getDataCallList(int32_t serial) {
    REQUIRE_RESP(resp);
    std::vector<radio::data::SetupDataCallResult> list;
    {
        std::lock_guard<std::mutex> l(h_.mu);
        if (h_.dataCall.active) list.push_back(resultFrom(h_.dataCall.dc, kCid));
    }
    resp->getDataCallListResponse(rspInfo(serial), list);
    return ::ndk::ScopedAStatus::ok();
}

/* Defaults to "allowed". Disallowing tears the active call down, which is what "data calls are not allowed" means. */
::ndk::ScopedAStatus RadioDataImpl::setDataAllowed(int32_t serial, bool allow) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, allow] {
        ril_data_call_t dc{};
        bool teardown = false;
        {
            std::lock_guard<std::mutex> l(h_.mu);
            h_.dataAllowed = allow;
            if (!allow && h_.dataCall.active) {
                dc = h_.dataCall.dc;
                h_.dataCall.active = false;
                teardown = true;
            }
        }
        int rc = 0;
        if (teardown && h_.ready()) rc = ril_data_stop(h_.ril, dc.handle, dc.ifname);
        resp->setDataAllowedResponse(rspInfo(serial, mapError(rc)));
        if (teardown) notifyDataStatus(false);
    });
    return ::ndk::ScopedAStatus::ok();
}

void RadioDataImpl::notifyDataStatus(bool connected) {
    auto ind = mInd;
    if (!ind) return;
    std::vector<radio::data::SetupDataCallResult> list;
    if (connected) return; /* a new call is reported through the setupDataCall response, not through a list that may still be empty */
    {
        std::lock_guard<std::mutex> l(h_.mu);
        h_.dataCall.active = false; /* the modem dropped the session (or we just tore it down) */
    }
    ind->dataCallListChanged(RadioIndicationType::UNSOLICITED, list);
}

}  // namespace j5ril
