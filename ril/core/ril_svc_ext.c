/* SPDX-License-Identifier: Apache-2.0
 * ril_svc_ext.c - WMS (SMS), VOICE, WDS (packet data), WDA and the network interface helpers.
 *
 * Verification status of the message layouts used here:
 *   WMS   [IDL] layouts cross-checked with libqril qril_wms.qmi (not tested against the real modem)
 *   VOICE [UNVERIFIED] written from memory of the Qualcomm voice service; never talked to a real modem
 *   WDS   [LIBQMI] ids/TLVs; the rmnet/bam-dmux data path of MSM8916 is [UNVERIFIED]
 * Nothing here has been exercised against the J5 modem. See README_RU.md for the status table. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ril_internal.h"

/* ---- WMS: layouts checked against libqril qril_wms.qmi ---- */
#define WMS_SET_EVENT_REPORT 0x0001
#define WMS_EVENT_REPORT_IND 0x0001
#define WMS_RAW_SEND 0x0020
#define WMS_SET_ROUTES 0x0032
#define WMS_GET_SMSC 0x0034
#define WMS_SEND_ACK 0x0037
#define WMS_FORMAT_3GPP 0x06

/* ---- VOICE ---- */
#define VOICE_INDICATION_REGISTER 0x0003
#define VOICE_DIAL_CALL 0x0020
#define VOICE_END_CALL 0x0021
#define VOICE_ANSWER_CALL 0x0022
#define VOICE_START_CONT_DTMF 0x0025
#define VOICE_STOP_CONT_DTMF 0x0026
#define VOICE_ALL_CALL_STATUS_IND 0x002E
#define VOICE_GET_ALL_CALL_INFO 0x002F

/* ---- WDS ---- */
#define WDS_START_NETWORK 0x0020
#define WDS_STOP_NETWORK 0x0021
#define WDS_PKT_STATUS_IND 0x0022
#define WDS_GET_CURRENT_SETTINGS 0x002D
#define WDS_GET_PKT_STATISTICS 0x0024
#define WDS_SET_IP_FAMILY_PREF 0x004D
#define WDS_BIND_MUX_DATA_PORT 0x00A2
#define QMI_DATA_ENDPOINT_BAM_DMUX 5

/* ---- WDA ---- */
#define WDA_GET_DATA_FORMAT 0x0021

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ------------------------------------------------------------------ */
/* WMS                                                                 */
/* ------------------------------------------------------------------ */
int ril_sms_send(ril_qmi_t *r, const uint8_t *pdu, size_t len, int *message_ref, int *error_class, int *cause)
{
    uint8_t tl[400], rsp[128];
    qmi_wbuf_t w;
    qmi_span_t sp, v;
    size_t n = 0;
    int rc;
    if (message_ref) *message_ref = 0;
    if (error_class) *error_class = 0;
    if (cause) *cause = 0;
    if (!pdu || len < 2 || len > 255) return -EINVAL;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    {
        uint8_t hdr[3] = {WMS_FORMAT_3GPP, (uint8_t)len, (uint8_t)(len >> 8)};
        size_t mark;
        qmi_tlv_begin(&w, 0x01, &mark);
        qmi_wbuf_bytes(&w, hdr, 3);
        qmi_wbuf_bytes(&w, pdu, len);
        qmi_tlv_end(&w, mark);
    }
    if (!qmi_wbuf_ok(&w)) return -EMSGSIZE;
    /* retries = 0 on purpose: a retried send could deliver the message twice */
    rc = ril_svcw(r, QMI_SVC_WMS, WMS_RAW_SEND, &w, 45000, 0, rsp, sizeof(rsp), &n);
    sp.p = rsp;
    sp.n = n;
    /* [IDL] raw_send_resp: message_id 0x01 (u16), cause_code 0x10 (u16), error_class 0x11 (u8) */
    if (qmi_tlv_find(sp, 0x01, &v) == 1 && v.n >= 2 && message_ref) *message_ref = v.p[0] | (v.p[1] << 8);
    if (qmi_tlv_find(sp, 0x10, &v) == 1 && v.n >= 2 && cause) *cause = v.p[0] | (v.p[1] << 8);
    if (qmi_tlv_find(sp, 0x11, &v) == 1 && v.n >= 1 && error_class) *error_class = v.p[0];
    return rc;
}

int ril_sms_ack(ril_qmi_t *r, int success, int cause)
{
    uint8_t tl[32];
    qmi_wbuf_t w;
    uint32_t txid;
    int pending;
    pthread_mutex_lock(&r->mu);
    txid = r->sms_txid;
    pending = r->sms_ack_pending;
    r->sms_ack_pending = 0;
    pthread_mutex_unlock(&r->mu);
    if (!pending) return 0; /* the modem did not ask for an acknowledgement: nothing to do */
    qmi_wbuf_init(&w, tl, sizeof(tl));
    {
        size_t mark;
        qmi_tlv_begin(&w, 0x01, &mark);
        qmi_wbuf_u32(&w, txid);
        qmi_wbuf_u8(&w, 0x01); /* 3GPP */
        qmi_wbuf_u8(&w, success ? 1 : 0);
        qmi_tlv_end(&w, mark);
    }
    if (!success) {
        qmi_tlv_put_u8(&w, 0x10, (uint8_t)cause); /* [IDL] failure_cause u8 */
    }
    return ril_svcw(r, QMI_SVC_WMS, WMS_SEND_ACK, &w, 0, 0, NULL, 0, NULL);
}

int ril_sms_smsc(ril_qmi_t *r, char *out, size_t cap)
{
    uint8_t rsp[128];
    size_t n = 0;
    qmi_span_t sp, v;
    int rc = ril_svc(r, QMI_SVC_WMS, WMS_GET_SMSC, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    if (cap) out[0] = '\0';
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x01, &v) == 1 && v.n >= 4) {
        size_t l = v.p[3];
        size_t o = 0;
        if (l > v.n - 4) l = v.n - 4;
        /* real modem: type "145" + digits that ALREADY start with '+' (seen as "++7904..."): add '+' only when it is missing */
        if (v.p[0] == '1' && v.p[1] == '4' && v.p[2] == '5' && l > 0 && v.p[4] != '+' && o + 1 < cap) out[o++] = '+';
        if (l + o + 1 > cap) l = cap - o - 1;
        memcpy(out + o, v.p + 4, l);
        out[o + l] = '\0';
        return out[0] ? 0 : -ENODATA;
    }
    return -ENODATA;
}

void ril_wms_register(struct ril_qmi *r)
{
    uint8_t tl[48];
    qmi_wbuf_t w;
    int rc;
    /* route every class of incoming 3GPP point-to-point SMS to us ("transfer only") */
    qmi_wbuf_init(&w, tl, sizeof(tl));
    {
        size_t mark, cls;
        qmi_tlv_begin(&w, 0x01, &mark);
        qmi_wbuf_u16(&w, 5);
        for (cls = 0; cls < 5; cls++) {
            qmi_wbuf_u8(&w, 0x00);        /* point to point */
            qmi_wbuf_u8(&w, (uint8_t)cls); /* class 0..3, 4 = none */
            qmi_wbuf_u8(&w, 0xFF);        /* storage: none */
            qmi_wbuf_u8(&w, 0x02);        /* transfer only */
        }
        qmi_tlv_end(&w, mark);
        qmi_tlv_put_u8(&w, 0x10, 1); /* also transfer status reports */
    }
    rc = ril_svcw(r, QMI_SVC_WMS, WMS_SET_ROUTES, &w, 0, 0, NULL, 0, NULL);
    if (rc) qmi_logf(QMI_LOG_WARN, "ril: WMS set routes failed (%d): incoming SMS may be stored instead of delivered", rc);
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x10, 1); /* report new MT messages */
    rc = ril_svcw(r, QMI_SVC_WMS, WMS_SET_EVENT_REPORT, &w, 0, 0, NULL, 0, NULL);
    if (rc) qmi_logf(QMI_LOG_WARN, "ril: WMS event report registration failed (%d): incoming SMS will not be seen", rc);
}

void ril_wms_on_indication(struct ril_qmi *r, const qmi_msg_t *m)
{
    qmi_span_t v;
    if (m->msg_id != WMS_EVENT_REPORT_IND) return;
    if (qmi_tlv_find(m->tlvs, 0x11, &v) == 1) {
        qmi_rd_t rd;
        unsigned ack_ind, fmt, len;
        uint32_t txid;
        const uint8_t *d;
        qmi_rd_init(&rd, v.p, v.n);
        ack_ind = qmi_rd_u8(&rd);
        txid = qmi_rd_u32(&rd);
        fmt = qmi_rd_u8(&rd);
        len = qmi_rd_u16(&rd);
        d = qmi_rd_bytes(&rd, len);
        if (!qmi_rd_ok(&rd) || !d) {
            qmi_logf(QMI_LOG_ERROR, "ril: malformed WMS MT message indication");
            return;
        }
        if (fmt != WMS_FORMAT_3GPP) {
            qmi_logf(QMI_LOG_WARN, "ril: dropping SMS with unsupported format 0x%02x", fmt);
            return;
        }
        if (len + 4 > sizeof(((struct ril_ev_item *)0)->data)) {
            qmi_logf(QMI_LOG_ERROR, "ril: SMS of %u bytes does not fit the event buffer, dropped", len);
            return;
        }
        {
            uint8_t buf[sizeof(((struct ril_ev_item *)0)->data)];
            buf[0] = (uint8_t)txid;
            buf[1] = (uint8_t)(txid >> 8);
            buf[2] = (uint8_t)(txid >> 16);
            buf[3] = (uint8_t)(txid >> 24);
            memcpy(buf + 4, d, len);
            ril_ev_push(r, EV_SMS, ack_ind == 0, buf, len + 4); /* ack_indicator 0 = "send ACK required" */
        }
    } else if (qmi_tlv_find(m->tlvs, 0x10, &v) == 1) {
        qmi_logf(QMI_LOG_WARN, "ril: SMS was stored on the modem instead of transferred (route not applied)");
    }
}

/* ------------------------------------------------------------------ */
/* VOICE                                                               */
/* ------------------------------------------------------------------ */
static int map_call_state(unsigned s)
{
    switch (s) {
    case 0x01: return RIL_CALL_DIALING;   /* origination */
    case 0x04: return RIL_CALL_DIALING;   /* CC in progress */
    case 0x02: return RIL_CALL_INCOMING;
    case 0x03: return RIL_CALL_ACTIVE;    /* conversation */
    case 0x05: return RIL_CALL_ALERTING;
    case 0x06: return RIL_CALL_HOLDING;
    case 0x07: return RIL_CALL_WAITING;
    default: return -1;                   /* setup/disconnecting/end: not shown to Android */
    }
}

/* Parses a call list. The TLV ids differ between the two messages:
 *   All Call Status indication (0x2E): calls 0x01, remote numbers 0x10   [from memory, UNVERIFIED]
 *   GET_ALL_CALL_INFO (0x2F):          calls 0x10, remote numbers 0x11   [calls TLV 0x10 seen on the real modem
 *                                       (empty list: 10 01 00 00); the number TLV 0x11 is from libqmi memory]
 * Returns 0 and replaces the cache, -EPROTO when the list is malformed (cache untouched). */
static int voice_update_cache(struct ril_qmi *r, qmi_span_t tlvs, uint8_t t_calls, uint8_t t_nums)
{
    ril_call_t list[RIL_MAX_CALLS];
    size_t cnt = 0;
    qmi_span_t v;
    unsigned num, i;
    qmi_rd_t rd;
    memset(list, 0, sizeof(list));
    if (qmi_tlv_find(tlvs, t_calls, &v) != 1) return -EPROTO;
    qmi_rd_init(&rd, v.p, v.n);
    num = qmi_rd_u8(&rd);
    for (i = 0; i < num && qmi_rd_ok(&rd); i++) {
        unsigned id = qmi_rd_u8(&rd), state = qmi_rd_u8(&rd), type = qmi_rd_u8(&rd), dir = qmi_rd_u8(&rd);
        unsigned mode = qmi_rd_u8(&rd), mpty = qmi_rd_u8(&rd), als = qmi_rd_u8(&rd);
        int st;
        (void)type; (void)mode; (void)als;
        if (!qmi_rd_ok(&rd)) return -EPROTO;
        st = map_call_state(state);
        if (st < 0 || cnt >= RIL_MAX_CALLS) continue;
        list[cnt].id = (int)id;
        list[cnt].state = st;
        list[cnt].incoming = dir == 2;
        list[cnt].multiparty = mpty != 0;
        cnt++;
    }
    if (!qmi_rd_ok(&rd)) return -EPROTO;
    if (qmi_tlv_find(tlvs, t_nums, &v) == 1) {
        qmi_rd_init(&rd, v.p, v.n);
        num = qmi_rd_u8(&rd);
        for (i = 0; i < num && qmi_rd_ok(&rd); i++) {
            unsigned id = qmi_rd_u8(&rd), pres = qmi_rd_u8(&rd), l = qmi_rd_u8(&rd);
            const uint8_t *d = qmi_rd_bytes(&rd, l);
            size_t k;
            (void)pres;
            if (!qmi_rd_ok(&rd) || !d) break;
            for (k = 0; k < cnt; k++)
                if ((unsigned)list[k].id == id) qmi_copy_str(list[k].number, sizeof(list[k].number), d, l);
        }
    }
    pthread_mutex_lock(&r->mu);
    memcpy(r->calls, list, sizeof(list));
    r->ncalls = cnt;
    pthread_mutex_unlock(&r->mu);
    return 0;
}

void ril_voice_on_indication(struct ril_qmi *r, const qmi_msg_t *m)
{
    if (m->msg_id != VOICE_ALL_CALL_STATUS_IND) return;
    if (voice_update_cache(r, m->tlvs, 0x01, 0x10) == 0) ril_ev_push(r, EV_CALLS, 0, NULL, 0);
    else qmi_logf(QMI_LOG_ERROR, "ril: malformed All Call Status indication");
}

static int voice_refresh(struct ril_qmi *r)
{
    uint8_t rsp[1024];
    size_t n = 0;
    qmi_span_t sp;
    int rc = ril_svc(r, QMI_SVC_VOICE, VOICE_GET_ALL_CALL_INFO, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    return voice_update_cache(r, sp, 0x10, 0x11);
}

void ril_voice_register(struct ril_qmi *r)
{
    uint8_t tl[8];
    qmi_wbuf_t w;
    int rc;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x10, 1); /* DTMF events; call status indications are expected without registration [UNVERIFIED] */
    rc = ril_svcw(r, QMI_SVC_VOICE, VOICE_INDICATION_REGISTER, &w, 0, 0, NULL, 0, NULL);
    if (rc) qmi_logf(QMI_LOG_WARN, "ril: VOICE indication registration failed (%d)", rc);
    (void)voice_refresh(r);
}

int ril_voice_dial(ril_qmi_t *r, const char *number, int *call_id)
{
    uint8_t tl[96], rsp[64];
    qmi_wbuf_t w;
    qmi_span_t sp, v;
    size_t n = 0, i, l;
    int rc;
    if (!number || !*number) return -EINVAL;
    l = strlen(number);
    if (l > 80) return -EINVAL;
    for (i = 0; i < l; i++)
        if (!((number[i] >= '0' && number[i] <= '9') || number[i] == '+' || number[i] == '*' || number[i] == '#' ||
              number[i] == ',' || number[i] == 'p' || number[i] == 'w'))
            return -EINVAL;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_str(&w, 0x01, number);
    qmi_tlv_put_u8(&w, 0x10, 0x00); /* call type: voice */
    /* never retried: a duplicate dial would place two calls */
    rc = ril_svcw(r, QMI_SVC_VOICE, VOICE_DIAL_CALL, &w, 30000, 0, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    if (call_id) *call_id = -1;
    if (qmi_tlv_find(sp, 0x10, &v) == 1 && v.n >= 1 && call_id) *call_id = v.p[0];
    (void)voice_refresh(r);
    ril_ev_push(r, EV_CALLS, 0, NULL, 0);
    return 0;
}

static int voice_simple(ril_qmi_t *r, uint16_t msg, int call_id)
{
    uint8_t tl[8];
    qmi_wbuf_t w;
    int rc;
    if (call_id < 0 || call_id > 255) return -EINVAL;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x01, (uint8_t)call_id);
    rc = ril_svcw(r, QMI_SVC_VOICE, msg, &w, 15000, 0, NULL, 0, NULL);
    if (rc == 0) {
        (void)voice_refresh(r);
        ril_ev_push(r, EV_CALLS, 0, NULL, 0);
    }
    return rc;
}

int ril_voice_hangup(ril_qmi_t *r, int call_id) { return voice_simple(r, VOICE_END_CALL, call_id); }
int ril_voice_answer(ril_qmi_t *r, int call_id) { return voice_simple(r, VOICE_ANSWER_CALL, call_id); }

int ril_voice_calls(ril_qmi_t *r, ril_call_t *out, size_t max, size_t *count)
{
    size_t n;
    int rc = voice_refresh(r);
    /* a failed refresh is not fatal when the cache is fed by indications; transport loss is */
    if (rc == -ENOTCONN) return rc;
    pthread_mutex_lock(&r->mu);
    n = r->ncalls < max ? r->ncalls : max;
    memcpy(out, r->calls, n * sizeof(out[0]));
    pthread_mutex_unlock(&r->mu);
    *count = n;
    return 0;
}

int ril_voice_dtmf(ril_qmi_t *r, int call_id, char digit, int start)
{
    uint8_t tl[8];
    qmi_wbuf_t w;
    if (call_id < 0 || call_id > 255) return -EINVAL;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    if (start) {
        uint8_t b[2];
        if (!((digit >= '0' && digit <= '9') || digit == '*' || digit == '#' || (digit >= 'A' && digit <= 'D'))) return -EINVAL;
        b[0] = (uint8_t)call_id;
        b[1] = (uint8_t)digit;
        qmi_tlv_put(&w, 0x01, b, 2);
        return ril_svcw(r, QMI_SVC_VOICE, VOICE_START_CONT_DTMF, &w, 0, 0, NULL, 0, NULL);
    }
    qmi_tlv_put_u8(&w, 0x01, (uint8_t)call_id);
    return ril_svcw(r, QMI_SVC_VOICE, VOICE_STOP_CONT_DTMF, &w, 0, 0, NULL, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* WDA: diagnostic only                                                */
/* ------------------------------------------------------------------ */
void ril_wda_setup(struct ril_qmi *r)
{
    uint8_t rsp[128];
    size_t n = 0;
    qmi_span_t sp, v;
    int rc;
    if (!r->cid[QMI_SVC_WDA]) return;
    rc = ril_svc(r, QMI_SVC_WDA, WDA_GET_DATA_FORMAT, NULL, 0, 0, 0, rsp, sizeof(rsp), &n);
    if (rc) {
        qmi_logf(QMI_LOG_INFO, "ril: WDA get data format failed (%d), leaving the data format untouched", rc);
        return;
    }
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x11, &v) == 1 && v.n >= 4)
        qmi_logf(QMI_LOG_INFO, "ril: WDA link layer protocol = %u (1 = 802.3, 2 = raw IP); not changed by this RIL", le32(v.p));
}

/* ------------------------------------------------------------------ */
/* WDS                                                                 */
/* ------------------------------------------------------------------ */
void ril_wds_on_indication(struct ril_qmi *r, const qmi_msg_t *m)
{
    qmi_span_t v;
    if (m->msg_id != WDS_PKT_STATUS_IND) return;
    if (qmi_tlv_find(m->tlvs, 0x01, &v) == 1 && v.n >= 1) ril_ev_push(r, EV_DATA, v.p[0] == 2, NULL, 0);
}

static void ip_from_wire(uint8_t out[4], const uint8_t *wire) /* wire is the little endian u32 of the host-order address */
{
    out[0] = wire[3];
    out[1] = wire[2];
    out[2] = wire[1];
    out[3] = wire[0];
}

int ril_data_find_iface(char *out, size_t cap)
{
    const char *env = getenv("RIL_DATA_IFACE");
    DIR *d;
    struct dirent *e;
    char best[IFNAMSIZ] = "";
    if (env && *env) {
        if (strlen(env) >= cap || strlen(env) >= IFNAMSIZ) return -ENAMETOOLONG;
        strcpy(out, env);
        return 0;
    }
    d = opendir("/sys/class/net");
    if (!d) return -errno;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "rmnet_data", 10) == 0 || strncmp(e->d_name, "rmnet", 5) == 0 || strncmp(e->d_name, "wwan", 4) == 0) {
            /* rmnet_ipa0 is the IPA control netdev, never a data session */
            if (strncmp(e->d_name, "rmnet_ipa", 9) == 0) continue;
            if (strlen(e->d_name) >= sizeof(best)) continue; /* not a valid interface name */
            if (!best[0] || strcmp(e->d_name, best) < 0) memcpy(best, e->d_name, strlen(e->d_name) + 1);
        }
    }
    closedir(d);
    if (!best[0]) return -ENODEV;
    if (strlen(best) >= cap) return -ENAMETOOLONG;
    strcpy(out, best);
    return 0;
}

static int iface_set(const char *ifname, int up, int mtu)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), rc = 0;
    if (s < 0) return -errno;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (mtu > 0) {
        ifr.ifr_mtu = mtu;
        if (ioctl(s, SIOCSIFMTU, &ifr) < 0) rc = -errno;
    }
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
        rc = -errno;
    } else {
        if (up) ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        else ifr.ifr_flags &= ~IFF_UP;
        if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) rc = -errno;
    }
    close(s);
    return rc;
}

int ril_data_start(ril_qmi_t *r, const ril_data_req_t *req, ril_data_call_t *out)
{
    uint8_t tl[320], rsp[256];
    qmi_wbuf_t w;
    qmi_span_t sp, v;
    size_t n = 0;
    int rc;
    memset(out, 0, sizeof(*out));
    if (!req || !req->apn[0]) return -EINVAL;
    rc = ril_data_find_iface(out->ifname, sizeof(out->ifname));
    if (rc) return rc;

    /* BAM-DMUX (netdev "wwan*"): the WDS client must be bound to the DMUX channel (endpoint type BAM_DMUX,
     * interface number = dev_port of the netdev), otherwise the modem has no data path and nothing flows.
     * Per libqmi/ModemManager: Bind Mux Data Port 0x00A2, TLV 0x10 {u32 endpoint type, u32 iface number}.
     * Failure is not fatal (other firmware binds differently); it is reported. */
    if (strncmp(out->ifname, "wwan", 4) == 0) {
        char pth[96], buf[16] = "";
        FILE *f;
        long port = 0;
        snprintf(pth, sizeof(pth), "/sys/class/net/%s/dev_port", out->ifname);
        f = fopen(pth, "r");
        if (f) { if (fgets(buf, sizeof(buf), f)) port = strtol(buf, NULL, 0); fclose(f); }
        {
            /* The modem answered 0x30 to the endpoint-only request; mux id / client type are optional in the IDL, but
             * try them in turn (all variants are harmless: they only configure this WDS client). The first success wins. */
            static const struct { int mux; int client; } var[] = {
                {-1, -1}, {0, -1}, {1, -1}, {0xFF, -1}, {-1, 1}, {1, 1}, {0xFF, 0xFF},
            };
            uint8_t ep[8];
            size_t vi;
            ep[0] = QMI_DATA_ENDPOINT_BAM_DMUX; ep[1] = ep[2] = ep[3] = 0;
            ep[4] = (uint8_t)port; ep[5] = (uint8_t)(port >> 8); ep[6] = ep[7] = 0;
            for (vi = 0; vi < sizeof(var) / sizeof(var[0]); vi++) {
                qmi_wbuf_init(&w, tl, sizeof(tl));
                qmi_tlv_put(&w, 0x10, ep, sizeof(ep));
                if (var[vi].mux >= 0) qmi_tlv_put_u8(&w, 0x11, (uint8_t)var[vi].mux);
                if (var[vi].client >= 0) qmi_tlv_put_u32(&w, 0x13, (uint32_t)var[vi].client);
                rc = ril_svcw(r, QMI_SVC_WDS, WDS_BIND_MUX_DATA_PORT, &w, 0, 0, NULL, 0, NULL);
                fprintf(stderr, "ril: WDS bind mux data port (BAM_DMUX, channel %ld, mux %d, client %d) -> %d\n",
                        port, var[vi].mux, var[vi].client, rc);
                if (rc == 0) break;
            }
        }
    }

    /* IPv4 only: the family preference is best effort, an unsupported command is not fatal */
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x01, 4);
    rc = ril_svcw(r, QMI_SVC_WDS, WDS_SET_IP_FAMILY_PREF, &w, 0, 0, NULL, 0, NULL);
    if (rc) qmi_logf(QMI_LOG_INFO, "ril: WDS set IP family preference failed (%d), continuing", rc);

    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_str(&w, 0x14, req->apn);
    if (req->user[0]) qmi_tlv_put_str(&w, 0x17, req->user);
    if (req->pass[0]) qmi_tlv_put_str(&w, 0x18, req->pass);
    if (req->auth > 0) qmi_tlv_put_u8(&w, 0x16, (uint8_t)req->auth);
    qmi_tlv_put_u8(&w, 0x19, 4);
    if (!qmi_wbuf_ok(&w)) return -EMSGSIZE;
    rc = ril_svcw(r, QMI_SVC_WDS, WDS_START_NETWORK, &w, 60000, 0, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x01, &v) != 1 || v.n < 4) return -EPROTO;
    out->handle = le32(v.p);

    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u32(&w, 0x10, 0x2310); /* DNS(1<<4), IP address(1<<8), gateway(1<<9), MTU(1<<13); 0x4610 returned no IP on the real modem */
    rc = ril_svcw(r, QMI_SVC_WDS, WDS_GET_CURRENT_SETTINGS, &w, 0, 1, rsp, sizeof(rsp), &n);
    if (rc) {
        qmi_logf(QMI_LOG_ERROR, "ril: data call started but settings unavailable (%d), stopping it", rc);
        (void)ril_data_stop(r, out->handle, NULL);
        memset(out, 0, sizeof(*out));
        return rc;
    }
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x1E, &v) == 1 && v.n >= 4) { ip_from_wire(out->ip, v.p); out->has_ip = 1; }
    if (qmi_tlv_find(sp, 0x21, &v) == 1 && v.n >= 4) ip_from_wire(out->mask, v.p);
    if (qmi_tlv_find(sp, 0x20, &v) == 1 && v.n >= 4) { ip_from_wire(out->gw, v.p); out->has_gw = 1; }
    if (qmi_tlv_find(sp, 0x15, &v) == 1 && v.n >= 4) { ip_from_wire(out->dns1, v.p); out->has_dns1 = 1; }
    if (qmi_tlv_find(sp, 0x16, &v) == 1 && v.n >= 4) { ip_from_wire(out->dns2, v.p); out->has_dns2 = 1; }
    if (qmi_tlv_find(sp, 0x29, &v) == 1 && v.n >= 4) out->mtu = (int)le32(v.p);
    if (!out->has_ip) {
        {   /* diagnostics: which TLVs did the modem actually send */
            size_t off = 0; uint8_t tt; qmi_span_t vv;
            fprintf(stderr, "ril: GET_CURRENT_SETTINGS has no IPv4 TLV 0x1E, got:");
            while (qmi_tlv_next(sp, &off, &tt, &vv) == 1) {
                fprintf(stderr, " [%02x len=%zu:", tt, vv.n);
                for (size_t i = 0; i < vv.n && i < 16; i++) fprintf(stderr, "%02x", vv.p[i]);
                fprintf(stderr, "]");
            }
            fprintf(stderr, "\n");
        }
        qmi_logf(QMI_LOG_ERROR, "ril: modem reported no IPv4 address, stopping the data call");
        (void)ril_data_stop(r, out->handle, NULL);
        memset(out, 0, sizeof(*out));
        return -ENODATA;
    }
    /* Addresses and routes are applied by the framework (netd) from the HAL result; here the link is only brought up. */
    rc = iface_set(out->ifname, 1, out->mtu);
    if (rc) {
        qmi_logf(QMI_LOG_ERROR, "ril: cannot bring %s up: %d", out->ifname, rc);
        (void)ril_data_stop(r, out->handle, NULL);
        memset(out, 0, sizeof(*out));
        return rc;
    }
    return 0;
}

int ril_data_stop(ril_qmi_t *r, uint32_t handle, const char *ifname)
{
    uint8_t tl[8];
    qmi_wbuf_t w;
    int rc;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u32(&w, 0x01, handle);
    rc = ril_svcw(r, QMI_SVC_WDS, WDS_STOP_NETWORK, &w, 30000, 0, NULL, 0, NULL);
    if (ifname && *ifname) (void)iface_set(ifname, 0, 0);
    /* an already dead session (invalid handle / out of call) counts as stopped */
    if (QMI_E_IS_QMI(rc) && (QMI_E_CODE(rc) == QMI_ERR_INVALID_HANDLE || QMI_E_CODE(rc) == QMI_ERR_OUT_OF_CALL)) return 0;
    return rc;
}

/* WDS packet statistics straight from the modem (diagnostics). The response TLVs are returned raw; the caller prints them.
 * The BAM-DMUX netdev of the kernel keeps no counters at all, so this is the only way to see whether the modem got packets. */
int ril_data_pkt_stats(ril_qmi_t *r, uint8_t *out, size_t cap, size_t *outlen)
{
    uint8_t tl[16];
    qmi_wbuf_t w;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u32(&w, 0x01, 0xFF); /* all counters (mask bits per libqmi, unverified) */
    return ril_svcw(r, QMI_SVC_WDS, WDS_GET_PKT_STATISTICS, &w, 0, 0, out, cap, outlen);
}
