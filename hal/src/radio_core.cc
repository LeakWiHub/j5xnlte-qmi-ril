// SPDX-License-Identifier: Apache-2.0
// IRadioConfig, IRadioModem, IRadioSim.
#define LOG_TAG "j5xnlte-ril"
#include <android/log.h>

#include <algorithm>

#include "radio_impl.h"

namespace j5ril {

using radio::RadioError;
using radio::RadioIndicationType;

#define REQUIRE_RESP(r)                       \
    auto r = mResp;                           \
    if (!r) return ::ndk::ScopedAStatus::ok()

/* ================================================================== IRadioSim */
bool RadioSimImpl::cardStatus(radio::sim::CardStatus* out, RadioError* err) {
    using namespace radio::sim;
    *err = RadioError::NONE;
    if (!h_.ready()) {
        *err = RadioError::RADIO_NOT_AVAILABLE;
        return false;
    }
    ril_card_t c;
    int rc = ril_sim_status(h_.ril, &c);
    if (rc) {
        *err = mapError(rc);
        return false;
    }
    CardStatus cs;
    cs.cardState = c.card_state == 1 ? CardStatus::STATE_PRESENT : c.card_state == 2 ? CardStatus::STATE_ERROR : CardStatus::STATE_ABSENT;
    cs.universalPinState = static_cast<PinState>(std::clamp(c.upin_state, 0, 5));
    cs.gsmUmtsSubscriptionAppIndex = -1;
    cs.cdmaSubscriptionAppIndex = -1;
    cs.imsSubscriptionAppIndex = -1;
    cs.slotMap.physicalSlotId = 0;
    cs.slotMap.portId = 0;
    if (c.present && c.has_app) {
        AppStatus a;
        a.appType = c.app_type;
        switch (c.app_state) { /* QmiUimCardApplicationState -> AppStatus.APP_STATE_* */
            case 1: a.appState = AppStatus::APP_STATE_DETECTED; break;
            case 2: a.appState = AppStatus::APP_STATE_PIN; break;
            case 3:
            case 5: a.appState = AppStatus::APP_STATE_PUK; break; /* PIN1 blocked == PUK required */
            case 4: a.appState = AppStatus::APP_STATE_SUBSCRIPTION_PERSO; break;
            case 7: a.appState = AppStatus::APP_STATE_READY; break;
            default: a.appState = AppStatus::APP_STATE_UNKNOWN;
        }
        a.persoSubstate = a.appState == AppStatus::APP_STATE_READY ? PersoSubstate::READY : PersoSubstate::UNKNOWN;
        a.aidPtr = toHex(c.aid, static_cast<size_t>(c.aid_len));
        a.pin1Replaced = c.upin_replaces_pin1 != 0;
        a.pin1 = static_cast<PinState>(std::clamp(c.pin1_state, 0, 5));
        a.pin2 = static_cast<PinState>(std::clamp(c.pin2_state, 0, 5));
        cs.applications.push_back(a);
        cs.gsmUmtsSubscriptionAppIndex = 0;
        if (a.appState == AppStatus::APP_STATE_READY) {
            char iccid[32];
            if (ril_sim_iccid(h_.ril, iccid, sizeof(iccid)) == 0) cs.iccid = iccid;
        }
    }
    *out = cs;
    return true;
}

::ndk::ScopedAStatus RadioSimImpl::getIccCardStatus(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        radio::sim::CardStatus cs;
        RadioError e;
        if (!cardStatus(&cs, &e)) cs = radio::sim::CardStatus();
        resp->getIccCardStatusResponse(rspInfo(serial, e), cs);
    });
    return ::ndk::ScopedAStatus::ok();
}

void RadioSimImpl::notifySimChanged() {
    auto ind = mInd;
    if (ind) ind->simStatusChanged(RadioIndicationType::UNSOLICITED);
}

::ndk::ScopedAStatus RadioSimImpl::iccIoForApp(int32_t serial, const radio::sim::IccIo& io) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, io] {
        radio::sim::IccIoResult res;
        std::vector<uint8_t> path;
        if (!h_.ready()) { (void)resp->iccIoForAppResponse(rspInfo(serial, RadioError::RADIO_NOT_AVAILABLE), res); return; }
        if (!fromHex(io.path, &path)) { (void)resp->iccIoForAppResponse(rspInfo(serial, RadioError::INVALID_ARGUMENTS), res); return; }
        if (path.empty()) {
            /* files directly below the MF are the only ones Android addresses without a path */
            if (io.fileId == 0x2FE2 || io.fileId == 0x2F00 || io.fileId == 0x2F05 || io.fileId == 0x2F06) path = {0x3F, 0x00};
            else { (void)resp->iccIoForAppResponse(rspInfo(serial, RadioError::INVALID_ARGUMENTS), res); return; }
        }
        ril_icc_result_t r;
        int rc;
        switch (io.command) {
            case 176: /* READ BINARY */
                rc = ril_sim_read_transparent(h_.ril, io.fileId, path.data(), path.size(), (io.p1 << 8) | io.p2,
                                              io.p3 ? io.p3 : 256, &r);
                break;
            case 178: /* READ RECORD (absolute mode) */
                rc = ril_sim_read_record(h_.ril, io.fileId, path.data(), path.size(), io.p1, io.p3 ? io.p3 : 256, &r);
                break;
            case 192: /* GET RESPONSE: the raw FCP template */
                rc = ril_sim_get_file_attrs(h_.ril, io.fileId, path.data(), path.size(), &r);
                break;
            default: /* UPDATE BINARY/RECORD, STATUS, SEEK ...: not implemented by this backend */
                { (void)resp->iccIoForAppResponse(rspInfo(serial, RadioError::REQUEST_NOT_SUPPORTED), res); return; }
        }
        if (rc == 0) {
            res.sw1 = r.sw1 ? r.sw1 : 0x90;
            res.sw2 = r.sw1 ? r.sw2 : 0x00;
            res.simResponse = toHex(r.data, r.len);
            { (void)resp->iccIoForAppResponse(rspInfo(serial), res); return; }
        }
        if (r.sw1) { /* the card answered with a status word (e.g. 6A82 file not found): that is a valid IccIoResult */
            res.sw1 = r.sw1;
            res.sw2 = r.sw2;
            { (void)resp->iccIoForAppResponse(rspInfo(serial), res); return; }
        }
        resp->iccIoForAppResponse(rspInfo(serial, mapError(rc)), res);
    });
    return ::ndk::ScopedAStatus::ok();
}

/* kind: 0 supplyPin, 1 supplyPuk, 2 changePin */
template <class F>
void RadioSimImpl::pinOp(int32_t serial, F op, int kind) {
    h_.exec.post([this, serial, op, kind] {
        auto resp = mResp;
        if (!resp) return;
        int left = -1, rc = h_.ready() ? op(&left) : -ENOTCONN;
        auto info = rspInfo(serial, mapError(rc));
        if (kind == 0) resp->supplyIccPinForAppResponse(info, left);
        else if (kind == 1) resp->supplyIccPukForAppResponse(info, left);
        else resp->changeIccPinForAppResponse(info, left);
    });
}

::ndk::ScopedAStatus RadioSimImpl::supplyIccPinForApp(int32_t serial, const std::string& pin, const std::string&) {
    REQUIRE_RESP(resp);
    (void)resp;
    pinOp(serial, [this, pin](int* left) { return ril_sim_verify_pin(h_.ril, RIL_PIN1, pin.c_str(), left); }, 0);
    return ::ndk::ScopedAStatus::ok();
}
::ndk::ScopedAStatus RadioSimImpl::supplyIccPukForApp(int32_t serial, const std::string& puk, const std::string& pin, const std::string&) {
    REQUIRE_RESP(resp);
    (void)resp;
    pinOp(serial, [this, puk, pin](int* left) { return ril_sim_unblock_pin(h_.ril, RIL_PIN1, puk.c_str(), pin.c_str(), left); }, 1);
    return ::ndk::ScopedAStatus::ok();
}
::ndk::ScopedAStatus RadioSimImpl::changeIccPinForApp(int32_t serial, const std::string& o, const std::string& n, const std::string&) {
    REQUIRE_RESP(resp);
    (void)resp;
    pinOp(serial, [this, o, n](int* left) { return ril_sim_change_pin(h_.ril, RIL_PIN1, o.c_str(), n.c_str(), left); }, 2);
    return ::ndk::ScopedAStatus::ok();
}

/* PIN2/PUK2 responses use their own response methods */
::ndk::ScopedAStatus RadioSimImpl::supplyIccPin2ForApp(int32_t serial, const std::string& pin2, const std::string&) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, pin2] {
        int left = -1, rc = h_.ready() ? ril_sim_verify_pin(h_.ril, RIL_PIN2, pin2.c_str(), &left) : -ENOTCONN;
        resp->supplyIccPin2ForAppResponse(rspInfo(serial, mapError(rc)), left);
    });
    return ::ndk::ScopedAStatus::ok();
}
::ndk::ScopedAStatus RadioSimImpl::supplyIccPuk2ForApp(int32_t serial, const std::string& puk2, const std::string& pin2, const std::string&) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, puk2, pin2] {
        int left = -1, rc = h_.ready() ? ril_sim_unblock_pin(h_.ril, RIL_PIN2, puk2.c_str(), pin2.c_str(), &left) : -ENOTCONN;
        resp->supplyIccPuk2ForAppResponse(rspInfo(serial, mapError(rc)), left);
    });
    return ::ndk::ScopedAStatus::ok();
}
::ndk::ScopedAStatus RadioSimImpl::changeIccPin2ForApp(int32_t serial, const std::string& o, const std::string& n, const std::string&) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, o, n] {
        int left = -1, rc = h_.ready() ? ril_sim_change_pin(h_.ril, RIL_PIN2, o.c_str(), n.c_str(), &left) : -ENOTCONN;
        resp->changeIccPin2ForAppResponse(rspInfo(serial, mapError(rc)), left);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSimImpl::getImsiForApp(int32_t serial, const std::string&) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        char imsi[32] = "";
        int rc = h_.ready() ? ril_sim_imsi(h_.ril, imsi, sizeof(imsi)) : -ENOTCONN;
        resp->getImsiForAppResponse(rspInfo(serial, mapError(rc)), rc ? "" : imsi);
    });
    return ::ndk::ScopedAStatus::ok();
}

/* Only the PIN1 lock ("SC") is backed by the UIM service; FDN, call barring etc. are not implemented. */
::ndk::ScopedAStatus RadioSimImpl::getFacilityLockForApp(int32_t serial, const std::string& facility, const std::string&, int32_t,
                                                         const std::string&) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, facility] {
        if (facility != "SC") { (void)resp->getFacilityLockForAppResponse(rspInfo(serial, RadioError::REQUEST_NOT_SUPPORTED), 0); return; }
        ril_card_t c;
        int rc = h_.ready() ? ril_sim_status(h_.ril, &c) : -ENOTCONN;
        if (rc == 0 && !c.has_app) rc = -ENODATA;
        if (rc) { (void)resp->getFacilityLockForAppResponse(rspInfo(serial, mapError(rc)), 0); return; }
        bool on = c.pin1_state == 1 || c.pin1_state == 2 || c.pin1_state == 4 || c.pin1_state == 5;
        resp->getFacilityLockForAppResponse(rspInfo(serial), on ? 1 : 0);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioSimImpl::setFacilityLockForApp(int32_t serial, const std::string& facility, bool lock, const std::string& password,
                                                         int32_t, const std::string&) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, facility, lock, password] {
        if (facility != "SC") { (void)resp->setFacilityLockForAppResponse(rspInfo(serial, RadioError::REQUEST_NOT_SUPPORTED), -1); return; }
        int left = -1, rc = h_.ready() ? ril_sim_set_pin_protection(h_.ril, RIL_PIN1, lock, password.c_str(), &left) : -ENOTCONN;
        resp->setFacilityLockForAppResponse(rspInfo(serial, mapError(rc)), left);
    });
    return ::ndk::ScopedAStatus::ok();
}

/* ================================================================== IRadioModem */
void RadioModemImpl::notifyRadioState() {
    auto ind = mInd;
    if (!ind) return;
    radio::modem::RadioState st = radio::modem::RadioState::UNAVAILABLE;
    if (h_.ready()) {
        std::lock_guard<std::mutex> l(h_.mu);
        st = h_.radioOn == 1 ? radio::modem::RadioState::ON : radio::modem::RadioState::OFF;
    }
    ind->radioStateChanged(RadioIndicationType::UNSOLICITED, st);
}

void RadioModemImpl::onResponseFunctionsSet() {
    h_.exec.post([this] {
        auto ind = mInd;
        if (ind) ind->rilConnected(RadioIndicationType::UNSOLICITED);
        notifyRadioState();
    });
}

::ndk::ScopedAStatus RadioModemImpl::getBasebandVersion(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        ril_ids_t ids;
        int rc = h_.ready() ? ril_modem_get_ids(h_.ril, &ids) : -ENOTCONN;
        resp->getBasebandVersionResponse(rspInfo(serial, mapError(rc)), rc ? "" : ids.revision);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModemImpl::getDeviceIdentity(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        ril_ids_t ids;
        int rc = h_.ready() ? ril_modem_get_ids(h_.ril, &ids) : -ENOTCONN;
        if (rc) { (void)resp->getDeviceIdentityResponse(rspInfo(serial, mapError(rc)), "", "", "", ""); return; }
        resp->getDeviceIdentityResponse(rspInfo(serial), ids.imei, ids.imeisv, ids.esn, ids.meid);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModemImpl::getImei(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        ril_ids_t ids;
        int rc = h_.ready() ? ril_modem_get_ids(h_.ril, &ids) : -ENOTCONN;
        if (rc || !ids.imei[0]) { (void)resp->getImeiResponse(rspInfo(serial, rc ? mapError(rc) : RadioError::GENERIC_FAILURE), std::nullopt); return; }
        radio::modem::ImeiInfo i;
        i.type = radio::modem::ImeiInfo::ImeiType::PRIMARY;
        i.imei = ids.imei;
        i.svn = ids.imeisv;
        resp->getImeiResponse(rspInfo(serial), i);
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModemImpl::getHardwareConfig(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([resp, serial] {
        radio::modem::HardwareConfig hc;
        hc.type = radio::modem::HardwareConfig::TYPE_MODEM;
        hc.uuid = "modem-0";
        hc.state = radio::modem::HardwareConfig::STATE_ENABLED;
        radio::modem::HardwareConfigModem m;
        m.rilModel = 0;
        m.rat = radio::RadioTechnology::LTE;
        m.maxVoiceCalls = 1; /* QMI voice: this RIL handles one call at a time */
        m.maxDataCalls = 1;
        m.maxStandby = 1;
        hc.modem.push_back(m);
        resp->getHardwareConfigResponse(rspInfo(serial), {hc});
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModemImpl::setRadioPower(int32_t serial, bool on, bool, bool) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial, on] {
        int rc = h_.ready() ? ril_modem_set_opmode(h_.ril, on ? RIL_OPMODE_ONLINE : RIL_OPMODE_LOW_POWER) : -ENOTCONN;
        if (rc == 0) {
            std::lock_guard<std::mutex> l(h_.mu);
            h_.radioOn = on ? 1 : 0;
        }
        resp->setRadioPowerResponse(rspInfo(serial, mapError(rc)));
        if (rc == 0) notifyRadioState();
    });
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioModemImpl::requestShutdown(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        int rc = h_.ready() ? ril_modem_set_opmode(h_.ril, RIL_OPMODE_LOW_POWER) : -ENOTCONN;
        resp->requestShutdownResponse(rspInfo(serial, mapError(rc)));
    });
    return ::ndk::ScopedAStatus::ok();
}

/* ================================================================== IRadioConfig */
std::vector<radio::config::SimSlotStatus> RadioConfigImpl::slots() {
    radio::config::SimSlotStatus s;
    radio::sim::CardStatus cs;
    RadioError e;
    s.cardState = radio::sim::CardStatus::STATE_ABSENT;
    if (h_.sim->cardStatus(&cs, &e)) s.cardState = cs.cardState;
    radio::config::SimPortInfo p;
    p.iccId = cs.iccid;
    p.logicalSlotId = 0;
    p.portActive = s.cardState == radio::sim::CardStatus::STATE_PRESENT;
    s.portInfo.push_back(p);
    return {s};
}

::ndk::ScopedAStatus RadioConfigImpl::getSimSlotsStatus(int32_t serial) {
    REQUIRE_RESP(resp);
    h_.exec.post([this, resp, serial] {
        if (!h_.ready()) { (void)resp->getSimSlotsStatusResponse(rspInfo(serial, RadioError::RADIO_NOT_AVAILABLE), {}); return; }
        resp->getSimSlotsStatusResponse(rspInfo(serial), slots());
    });
    return ::ndk::ScopedAStatus::ok();
}

void RadioConfigImpl::notifySlotsChanged() {
    auto ind = mInd;
    if (!ind) return;
    h_.exec.post([this, ind] {
        if (h_.ready()) ind->simSlotsStatusChanged(RadioIndicationType::UNSOLICITED, slots());
    });
}

::ndk::ScopedAStatus RadioConfigImpl::getHalDeviceCapabilities(int32_t serial) {
    REQUIRE_RESP(resp);
    /* true == "reduced feature set": tells the framework not to expect the optional modem features this RIL lacks */
    resp->getHalDeviceCapabilitiesResponse(rspInfo(serial), true);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioConfigImpl::getNumOfLiveModems(int32_t serial) {
    REQUIRE_RESP(resp);
    resp->getNumOfLiveModemsResponse(rspInfo(serial), 1);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioConfigImpl::getPhoneCapability(int32_t serial) {
    REQUIRE_RESP(resp);
    radio::config::PhoneCapability pc;
    pc.maxActiveData = 1;
    pc.maxActiveInternetData = 1;
    pc.isInternetLingeringSupported = false;
    pc.logicalModemIds = {0};
    pc.maxActiveVoice = 1;
    resp->getPhoneCapabilityResponse(rspInfo(serial), pc);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus RadioConfigImpl::setPreferredDataModem(int32_t serial, int8_t modemId) {
    REQUIRE_RESP(resp);
    /* one modem: only modem 0 can be preferred; anything else is an argument error, not a silent success */
    resp->setPreferredDataModemResponse(rspInfo(serial, modemId == 0 ? RadioError::NONE : RadioError::INVALID_ARGUMENTS));
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace j5ril
