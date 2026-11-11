/* SPDX-License-Identifier: Apache-2.0
 * test_ril_glue.c - drives ril_qmi.h against tests/fake_modem/fake_ril_modem.py over a pty.
 * usage: test_ril_glue <device>   (run through fake_ril_modem.py, which also checks the modem side) */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ril_qmi.h"

static int fails, total;
#define CHECK(cond, ...)                                          \
    do {                                                          \
        total++;                                                  \
        if (!(cond)) {                                            \
            fails++;                                              \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        } else {                                                  \
            printf("  ok   ");                                    \
            printf(__VA_ARGS__);                                  \
            printf("\n");                                         \
        }                                                         \
    } while (0)

static atomic_int g_ready, g_sim, g_net, g_sig, g_sms, g_calls, g_data, g_ack_req;
static atomic_int g_not_ready;
static uint8_t g_sms_pdu[300];
static size_t g_sms_len;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void on_state(void *c, int up) { (void)c; if (up) atomic_fetch_add(&g_ready, 1); else atomic_fetch_add(&g_not_ready, 1); }
static void on_sim(void *c) { (void)c; atomic_fetch_add(&g_sim, 1); }
static void on_net(void *c) { (void)c; atomic_fetch_add(&g_net, 1); }
static void on_sig(void *c) { (void)c; atomic_fetch_add(&g_sig, 1); }
static void on_calls(void *c) { (void)c; atomic_fetch_add(&g_calls, 1); }
static void on_data(void *c, int up) { (void)c; atomic_fetch_add(&g_data, up ? 1 : 100); }
static void on_sms(void *c, const uint8_t *p, size_t n, int ack)
{
    (void)c;
    pthread_mutex_lock(&g_mu);
    g_sms_len = n < sizeof(g_sms_pdu) ? n : sizeof(g_sms_pdu);
    memcpy(g_sms_pdu, p, g_sms_len);
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_ack_req, ack);
    atomic_fetch_add(&g_sms, 1);
}

static int wait_for(atomic_int *v, int at_least, int ms)
{
    int i;
    for (i = 0; i < ms / 10; i++) {
        if (atomic_load(v) >= at_least) return 1;
        usleep(10000);
    }
    return atomic_load(v) >= at_least;
}

static const uint8_t SEND_PDU[] = {0x00, 0x11, 0x00, 0x07, 0x91, 0x64, 0x07, 0x28, 0x15, 0x53, 0xF8, 0x00, 0x00, 0xAA, 0x0A,
                                   0xE8, 0x32, 0x9B, 0xFD, 0x46, 0x97, 0xD9, 0xEC, 0x37};
static const uint8_t MT_SMS[] = {0x07, 0x91, 0x44, 0x87, 0x20, 0x00, 0x30, 0x23, 0x04, 0x0B, 0x91, 0x64, 0x07, 0x28, 0x15,
                                 0x53, 0xF8, 0x00, 0x00, 0xFF, 0x0A, 0xE8, 0x32, 0x9B, 0xFD, 0x46, 0x97, 0xD9, 0xEC, 0x37};

int main(int argc, char **argv)
{
    ril_events_t ev = {on_state, on_sim, on_net, on_sig, on_sms, on_calls, on_data, NULL};
    const uint8_t mf_adf[] = {0x3F, 0x00, 0x7F, 0xFF};
    ril_qmi_t *r;
    int rc;
    if (argc < 2) return 2;
    setenv("RIL_DATA_IFACE", "lo", 1); /* the fake reports MTU 65536 == lo's MTU: bring-up is a no-op, nothing is changed */

    r = ril_qmi_create(argv[1], &ev);
    CHECK(r != NULL, "ril_qmi_create");
    if (!r) return 1;
    CHECK(ril_qmi_wait_ready(r, 8000) == 0, "modem becomes ready (clients allocated, indications registered)");
    CHECK(wait_for(&g_ready, 1, 1000), "on_modem_state(ready) delivered");

    /* DMS */
    {
        ril_ids_t ids;
        rc = ril_modem_get_ids(r, &ids);
        CHECK(rc == 0 && !strcmp(ids.imei, "351234567890123"), "IMEI '%s' rc=%d", ids.imei, rc);
        CHECK(!strcmp(ids.revision, "MPSS.TH.2.0-FAKE"), "revision '%s'", ids.revision);
    }
    /* UIM */
    {
        ril_card_t c;
        char s[32];
        ril_icc_result_t res;
        int left = 99;
        {   /* the fake modem starts like the real one (USIM "detected", unprovisioned); the core must provision it by itself */
            int tries;
            for (tries = 0; tries < 30; tries++) {
                rc = ril_sim_status(r, &c);
                if (rc == 0 && c.app_state == 7) break;
                usleep(100000);
            }
        }
        CHECK(rc == 0 && c.present && c.has_app && c.app_type == 2 && c.app_state == 7 && c.provisioned, "card: present USIM READY + provisioned automatically rc=%d type=%d state=%d",
              rc, c.app_type, c.app_state);
        CHECK(c.aid_len == 7 && c.aid[0] == 0xA0, "AID length %d", c.aid_len);
        CHECK(c.pin1_state == 1 && c.pin1_retries == 3 && c.puk1_retries == 10, "pin1 state/retries %d/%d/%d", c.pin1_state,
              c.pin1_retries, c.puk1_retries);
        CHECK(ril_sim_imsi(r, s, sizeof(s)) == 0 && !strcmp(s, "250011234567890"), "IMSI '%s'", s);
        CHECK(ril_sim_iccid(r, s, sizeof(s)) == 0 && !strcmp(s, "8970101234567890123"), "ICCID '%s'", s);
        rc = ril_sim_read_transparent(r, 0x6F07, mf_adf, sizeof(mf_adf), 0, 9, &res);
        CHECK(rc == 0 && res.len == 9 && res.data[0] == 0x08 && res.sw1 == 0x90, "EF_IMSI read rc=%d len=%zu sw=%02x%02x", rc, res.len, res.sw1, res.sw2);
        rc = ril_sim_get_file_attrs(r, 0x6F07, mf_adf, sizeof(mf_adf), &res);
        CHECK(rc == 0 && res.len == 24 && res.data[0] == 0x62, "file attributes raw FCP rc=%d len=%zu", rc, res.len);
        rc = ril_sim_verify_pin(r, RIL_PIN1, "0000", &left);
        CHECK(QMI_E_IS_QMI(rc) && QMI_E_CODE(rc) == QMI_ERR_INCORRECT_PIN && left == 2, "wrong PIN -> INCORRECT_PIN, retries left %d", left);
        CHECK(ril_sim_verify_pin(r, RIL_PIN1, "12", &left) == -EINVAL, "short PIN rejected locally");
        CHECK(ril_sim_verify_pin(r, RIL_PIN1, "1234", &left) == 0, "right PIN accepted");
        CHECK(wait_for(&g_sim, 1, 3000), "UIM status change indication -> on_sim_changed");
    }
    /* NAS */
    {
        ril_serving_t s;
        ril_signal_t sg;
        /* the modem's first answer is what the real one sends without service: result FAILURE/INFO_UNAVAILABLE + TLV 0x01 */
        rc = ril_net_serving(r, &s);
        CHECK(rc == 0 && s.reg_state == 0 && !s.plmn_valid && s.rat == RIL_RAT_NONE,
              "INFO_UNAVAILABLE from the real modem = not registered, not an error (rc=%d reg=%d)", rc, s.reg_state);
        rc = ril_net_serving(r, &s);
        CHECK(rc == 0 && s.reg_state == 1 && s.rat == RIL_RAT_LTE && s.cs_attach == 1 && s.ps_attach == 1, "serving: reg=%d rat=%d rc=%d", s.reg_state, s.rat, rc);
        CHECK(s.plmn_valid && s.mcc == 250 && s.mnc == 1 && !strcmp(s.operator_name, "MTS"), "PLMN %d/%d '%s'", s.mcc, s.mnc, s.operator_name);
        CHECK(s.lac_valid && s.lac == 0x1234 && s.cid_valid && s.cid == 0xBEEF, "LAC/CID 0x%x/0x%x", s.lac, s.cid);
        rc = ril_net_signal(r, &sg);
        CHECK(rc == 0 && sg.lte_valid && sg.lte_rsrp == -95 && sg.lte_rssi == -60 && sg.lte_rsrq == -9 && sg.lte_snr == 120,
              "LTE signal rssi=%d rsrq=%d rsrp=%d snr=%d", sg.lte_rssi, sg.lte_rsrq, sg.lte_rsrp, sg.lte_snr);
        CHECK(ril_net_register_auto(r) == 0, "automatic registration request");
        CHECK(wait_for(&g_net, 2, 3000), "serving system indication -> on_network_changed (%d)", atomic_load(&g_net));
        CHECK(wait_for(&g_sig, 1, 3000), "signal indication -> on_signal_changed");
    }
    /* WMS */
    {
        int ref = -1, cls = -1, cause = -1;
        uint8_t bad[sizeof(SEND_PDU)];
        char smsc[32];
        rc = ril_sms_send(r, SEND_PDU, sizeof(SEND_PDU), &ref, &cls, &cause);
        CHECK(rc == 0 && ref == 7, "SMS sent, modem message id %d rc=%d", ref, rc);
        memcpy(bad, SEND_PDU, sizeof(bad));
        bad[sizeof(bad) - 1] = 0xFF;
        rc = ril_sms_send(r, bad, sizeof(bad), &ref, &cls, &cause);
        CHECK(QMI_E_IS_QMI(rc) && cause == 0x123 && cls == 2, "failed send reports QMI error, cause 0x%x class %d", cause, cls);
        CHECK(ril_sms_send(r, SEND_PDU, 1, &ref, &cls, &cause) == -EINVAL, "too short PDU rejected locally");
        CHECK(wait_for(&g_sms, 1, 4000), "incoming SMS delivered");
        pthread_mutex_lock(&g_mu);
        CHECK(g_sms_len == sizeof(MT_SMS) && !memcmp(g_sms_pdu, MT_SMS, sizeof(MT_SMS)), "incoming SMS bytes intact (%zu)", g_sms_len);
        pthread_mutex_unlock(&g_mu);
        CHECK(atomic_load(&g_ack_req) == 1, "ack required flag");
        CHECK(ril_sms_ack(r, 1, 0) == 0, "ack sent");
        CHECK(ril_sms_ack(r, 1, 0) == 0, "second ack is a no-op (nothing pending)");
        CHECK(ril_sms_smsc(r, smsc, sizeof(smsc)) == 0 && !strcmp(smsc, "+79037029984"), "SMSC '%s'", smsc);
    }
    /* VOICE */
    {
        ril_call_t cl[RIL_MAX_CALLS];
        size_t n = 0, i;
        int id = -1, before;
        before = atomic_load(&g_calls);
        rc = ril_voice_dial(r, "+79001234567", &id);
        CHECK(rc == 0 && id == 1, "dial -> call id %d rc=%d", id, rc);
        CHECK(ril_voice_dial(r, "12;rm", &id) == -EINVAL, "dial string with junk rejected locally");
        CHECK(wait_for(&g_calls, before + 3, 4000), "call status indications delivered (%d)", atomic_load(&g_calls) - before);
        usleep(300000);
        rc = ril_voice_calls(r, cl, RIL_MAX_CALLS, &n);
        CHECK(rc == 0 && n == 1 && cl[0].state == RIL_CALL_ACTIVE && !cl[0].incoming && !strcmp(cl[0].number, "+79001234567"),
              "outgoing call ACTIVE n=%zu state=%d", n, n ? cl[0].state : -1);
        CHECK(ril_voice_dtmf(r, 1, '5', 1) == 0 && ril_voice_dtmf(r, 1, '5', 0) == 0, "DTMF start/stop");
        before = atomic_load(&g_calls);
        CHECK(ril_voice_hangup(r, 1) == 0, "hangup");
        CHECK(wait_for(&g_calls, before + 3, 4000), "hangup + incoming call indications delivered");
        usleep(200000);
        rc = ril_voice_calls(r, cl, RIL_MAX_CALLS, &n);
        for (i = 0; i < n; i++) if (cl[i].id == 2) break;
        CHECK(rc == 0 && i < n && cl[i].state == RIL_CALL_INCOMING && cl[i].incoming && !strcmp(cl[i].number, "+79001234567"),
              "incoming call visible n=%zu", n);
        CHECK(ril_voice_answer(r, 2) == 0, "answer");
        rc = ril_voice_calls(r, cl, RIL_MAX_CALLS, &n);
        CHECK(rc == 0 && n == 1 && cl[0].state == RIL_CALL_ACTIVE, "answered call ACTIVE");
        CHECK(ril_voice_hangup(r, 2) == 0, "hangup 2");
    }
    /* WDS */
    {
        ril_data_req_t rq;
        ril_data_call_t dc;
        memset(&rq, 0, sizeof(rq));
        snprintf(rq.apn, sizeof(rq.apn), "internet.test");
        snprintf(rq.user, sizeof(rq.user), "user");
        snprintf(rq.pass, sizeof(rq.pass), "pass");
        rq.auth = 3;
        rq.ip_family = 4;
        rc = ril_data_start(r, &rq, &dc);
        CHECK(rc == 0 && dc.handle == 0xCAFE0001u && !strcmp(dc.ifname, "lo"), "data call started rc=%d handle=%x if=%s", rc, dc.handle, dc.ifname);
        CHECK(dc.has_ip && dc.ip[0] == 10 && dc.ip[1] == 20 && dc.ip[2] == 30 && dc.ip[3] == 40, "IPv4 octet order %u.%u.%u.%u", dc.ip[0], dc.ip[1], dc.ip[2], dc.ip[3]);
        CHECK(dc.has_gw && dc.gw[3] == 1 && dc.mask[0] == 255 && dc.mask[3] == 0, "gateway/mask");
        CHECK(dc.has_dns1 && dc.dns1[0] == 8 && dc.dns1[3] == 8 && dc.dns2[2] == 4, "DNS");
        CHECK(dc.mtu == 65536, "MTU %d", dc.mtu);
        CHECK(wait_for(&g_data, 1, 3000), "packet status indication -> on_data_status(connected)");
        CHECK(ril_data_stop(r, dc.handle, NULL) == 0, "data call stopped");
        CHECK(wait_for(&g_data, 101, 3000), "disconnect indication delivered");
    }
    ril_qmi_destroy(r);
    printf("\n[glue test] %d checks, %d failed\n", total, fails);
    return fails ? 1 : 0;
}
