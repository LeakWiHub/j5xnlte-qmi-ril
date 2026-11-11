/* SPDX-License-Identifier: Apache-2.0
 * test_ril_real.c - feeds RESPONSES RECORDED FROM THE REAL MODEM (tests/fake_modem/replay_real.py) through the
 * ril_qmi.h API. This is the only test whose input bytes were not produced by me. It still is not a device test:
 * the HAL, binder, init and SELinux were not involved, and only the six recorded requests exist. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ril_qmi.h"

static int fails, total;
#define CHECK(cond, ...) do { total++; if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } else { printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    ril_events_t ev;
    ril_qmi_t *r;
    ril_ids_t ids;
    ril_card_t card;
    ril_serving_t sv;
    ril_signal_t sg;
    int mode = -1, rc;
    if (argc < 2) return 2;
    memset(&ev, 0, sizeof(ev));
    r = ril_qmi_create(argv[1], &ev);
    CHECK(r != NULL, "ril_qmi_create");
    if (!r) return 1;
    CHECK(ril_qmi_wait_ready(r, 8000) == 0, "ready despite 8 CTL sync indications queued before the first answer");

    memset(&ids, 0, sizeof(ids));
    rc = ril_modem_get_ids(r, &ids);
    CHECK(rc == 0, "DMS get ids (real frame) rc=%d", rc);
    CHECK(strcmp(ids.imei, "350000000000008") == 0, "IMEI parsed from TLV 0x11 (got '%s')", ids.imei);
    CHECK(strcmp(ids.esn, "B0000000") == 0, "ESN parsed (got '%s')", ids.esn);
    CHECK(strncmp(ids.revision, "MPSS.DPM.2.0.2.c1.7", 19) == 0, "revision parsed (got '%s')", ids.revision);

    {
    int online = getenv("J5_REPLAY") && !strcmp(getenv("J5_REPLAY"), "online");
    int sim = getenv("J5_REPLAY") && !strcmp(getenv("J5_REPLAY"), "sim");
    int reg = getenv("J5_REPLAY") && !strcmp(getenv("J5_REPLAY"), "registered");
    printf("scenario: %s\n", reg ? "SIM ready, registered on LTE" : online ? "online, searching (no SIM)" : sim ? "SIM inserted, right after boot" : "first run, modem not online");
    rc = ril_modem_get_opmode(r, &mode);
    CHECK(rc == 0 && mode == ((online || reg) ? 0 : 5), "DMS operating mode as the real modem reported (rc=%d mode=%d)", rc, mode);

    memset(&card, 0xAA, sizeof(card));
    rc = ril_sim_status(r, &card);
    CHECK(rc == 0, "UIM card status (real frame) rc=%d", rc);
    if (sim || reg) {
        int tries;   /* the core provisions by itself at READY; the real modem then reports the USIM READY */
        for (tries = 0; tries < 30 && !(rc == 0 && card.app_state == 7); tries++) {
            usleep(100000);
            rc = ril_sim_status(r, &card);
        }
        CHECK(card.card_state == 1 && card.present, "card present (state=%d present=%d)", card.card_state, card.present);
        CHECK(card.has_app && card.app_type == 2 && card.app_state == 7 && card.provisioned,
              "USIM READY after the automatic UIM 0x38 (has=%d type=%d state=%d provisioned=%d)", card.has_app, card.app_type, card.app_state, card.provisioned);
        CHECK(card.pin1_state == 3 && card.pin1_retries == 3 && card.puk1_retries == 10, "PIN1 disabled (3), retries 3/10 (got %d %d/%d)", card.pin1_state, card.pin1_retries, card.puk1_retries);
        CHECK(card.aid_len == 16 && card.aid[0] == 0xA0 && card.aid[1] == 0x00 && card.aid[4] == 0x87 && card.aid[5] == 0x10, "AID of 16 bytes A0 00 00 00 87 10 ... (len=%d)", card.aid_len);
    } else {
        CHECK(card.card_state == 2 && !card.present && !card.has_app, "card in ERROR state, no app (state=%d present=%d app=%d)",
              card.card_state, card.present, card.has_app);
    }

    memset(&sv, 0, sizeof(sv));
    rc = ril_net_serving(r, &sv);
    CHECK(rc == 0, "NAS serving system rc=%d", rc);
    if (reg) {
        CHECK(sv.reg_state == 1 && sv.cs_attach == 1 && sv.ps_attach == 1, "registered, CS/PS attached (reg=%d cs=%d ps=%d)", sv.reg_state, sv.cs_attach, sv.ps_attach);
        CHECK(sv.rat == RIL_RAT_LTE, "radio interface LTE (rat=%d)", sv.rat);
        CHECK(sv.plmn_valid && sv.mcc == 250 && sv.mnc == 20, "PLMN 250/20 (valid=%d %d/%d)", sv.plmn_valid, sv.mcc, sv.mnc);
        CHECK(!strcmp(sv.operator_name, "ROSTELECOM"), "operator name decoded from GSM 7-bit packed (got '%s')", sv.operator_name);
        CHECK(sv.cid_valid && sv.cid == 0x09513C0Cu && !sv.lac_valid, "cell id 0x09513C0C from TLV 0x1D, LAC 0xFFFE = none (cid_valid=%d cid=%#x lac_valid=%d)", sv.cid_valid, sv.cid, sv.lac_valid);
    } else if (online) {
        CHECK(sv.reg_state == 2 && sv.cs_attach == 2 && sv.ps_attach == 2, "searching, CS/PS detached (reg=%d cs=%d ps=%d)", sv.reg_state, sv.cs_attach, sv.ps_attach);
        CHECK(sv.rat == RIL_RAT_UMTS, "radio interface UMTS (rat=%d)", sv.rat);
        CHECK(sv.plmn_valid && sv.mcc == 250 && sv.mnc == 99, "PLMN 250/99 from TLV 0x12 (valid=%d %d/%d)", sv.plmn_valid, sv.mcc, sv.mnc);
    } else {
        CHECK(sv.reg_state == 0, "reg_state = not registered (got %d, 4 would mean the body was lost)", sv.reg_state);
    }

    memset(&sg, 0, sizeof(sg));
    rc = ril_net_signal(r, &sg);
    CHECK(rc == 0, "NAS signal info (0x4F) rc=%d", rc);
    if (reg) {
        CHECK(sg.lte_valid && sg.lte_rssi == -81 && sg.lte_rsrq == -9 && sg.lte_rsrp == -110 && sg.lte_snr == 22, "LTE signal from TLV 0x14: rssi -81 rsrq -9 rsrp -110 snr 22 (valid=%d %d/%d/%d/%d)", sg.lte_valid, sg.lte_rssi, sg.lte_rsrq, sg.lte_rsrp, sg.lte_snr);
    } else if (online) {
        CHECK(sg.wcdma_valid && sg.wcdma_rssi == -88 && sg.wcdma_ecio == 10, "WCDMA signal from TLV 0x13: rssi -88, ecio raw 10 (valid=%d rssi=%d ecio=%d)", sg.wcdma_valid, sg.wcdma_rssi, sg.wcdma_ecio);
        CHECK(!sg.gsm_valid && !sg.lte_valid, "no GSM/LTE signal reported");
    } else {
        CHECK(!sg.gsm_valid && !sg.wcdma_valid && !sg.lte_valid && !sg.generic_valid, "no signal reported (0x4F INFO_UNAVAILABLE = no signal, not an error)");
    }
    }

    {
        unsigned mask = 0;
        rc = ril_net_get_mode_pref(r, &mask);
        CHECK(rc == 0 && mask == (RIL_RATMASK_GSM | RIL_RATMASK_UMTS | RIL_RATMASK_LTE),
              "mode preference from the real answer (TLV 0x11 = 0x001C) = GSM|UMTS|LTE (rc=%d mask=%#x)", rc, mask);
    }
    {
        ril_call_t calls[RIL_MAX_CALLS];
        size_t cnt = 99;
        rc = ril_voice_calls(r, calls, RIL_MAX_CALLS, &cnt);
        CHECK(rc == 0 && cnt == 0, "VOICE get all call info: real answer carries the (empty) call list in TLV 0x10 (rc=%d count=%zu)", rc, cnt);
    }

    ril_qmi_destroy(r);
    printf("\n[real-frames test] %d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
