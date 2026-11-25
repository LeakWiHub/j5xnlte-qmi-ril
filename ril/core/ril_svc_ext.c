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
#include <netinet/in.h>
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
#define VOICE_ORIG_USSD 0x003A
#define VOICE_ANSWER_USSD 0x003B
#define VOICE_CANCEL_USSD 0x003C
#define VOICE_USSD_RELEASE_IND 0x003D
#define VOICE_USSD_IND 0x003E
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
#define WDS_BIND_DATA_PORT     0x0089
#define SIO_PORT_A2_MUX_RMNET0 0x0e04   /* BAM-DMUX channel n = SIO port 0x0e04 + n (libqmi QmiSioPort) */
#define QMI_DATA_ENDPOINT_BAM_DMUX 5

/* ---- WDA ---- */
#define WDA_SET_DATA_FORMAT 0x0020
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
    else qmi_logf(QMI_LOG_WARN, "ril: WMS routes set (transfer only)");
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x10, 1); /* report new MT messages */
    rc = ril_svcw(r, QMI_SVC_WMS, WMS_SET_EVENT_REPORT, &w, 0, 0, NULL, 0, NULL);
    if (rc) qmi_logf(QMI_LOG_WARN, "ril: WMS event report registration failed (%d): incoming SMS will not be seen", rc);
    else qmi_logf(QMI_LOG_WARN, "ril: WMS event report registered");
}

void ril_wms_on_indication(struct ril_qmi *r, const qmi_msg_t *m)
{
    qmi_span_t v;
    qmi_logf(QMI_LOG_WARN, "ril: WMS indication 0x%04x (event report is 0x%04x)", m->msg_id, WMS_EVENT_REPORT_IND);
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
            qmi_logf(QMI_LOG_WARN, "ril: incoming SMS %u bytes, ack required %d", len, ack_ind == 0);
            ril_ev_push(r, EV_SMS, ack_ind == 0, buf, len + 4); /* ack_indicator 0 = "send ACK required" */
        }
    } else if (qmi_tlv_find(m->tlvs, 0x10, &v) == 1) {
        qmi_logf(QMI_LOG_WARN, "ril: SMS was stored on the modem instead of transferred (route not applied), reading it from storage");
        if (v.n >= 5) ril_ev_push(r, EV_SMS_STORED, 0, v.p, 5);
    }
}

/* Fallback for a message the modem stored instead of transferring. Raw Read 0x0022 per libqmi (NOT verified on this
 * modem): in 0x01 {storage u8, index u32}, 0x10 message mode u8 (1 = GSM/WCDMA); out 0x01 {tag u8, format u8,
 * len u16, data}. Runs on the worker thread (a QMI request cannot be made from the reader thread). */
void ril_wms_read_stored(struct ril_qmi *r, const uint8_t *loc, size_t len)
{
    uint8_t tl[24], rsp[512];
    qmi_wbuf_t w;
    qmi_span_t sp, v;
    size_t n = 0;
    int rc;
    if (len < 5) return;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put(&w, 0x01, loc, 5);
    qmi_tlv_put_u8(&w, 0x10, 1);
    rc = ril_svcw(r, QMI_SVC_WMS, 0x0022, &w, 0, 0, rsp, sizeof(rsp), &n);
    qmi_logf(QMI_LOG_WARN, "ril: WMS raw read of stored SMS (storage %u index %u) -> %d", loc[0],
             (unsigned)loc[1] | ((unsigned)loc[2] << 8) | ((unsigned)loc[3] << 16) | ((unsigned)loc[4] << 24), rc);
    if (rc) return;
    sp.p = rsp; sp.n = n;
    if (qmi_tlv_find(sp, 0x01, &v) == 1 && v.n >= 4) {
        unsigned fmt = v.p[1], dl = (unsigned)v.p[2] | ((unsigned)v.p[3] << 8);
        if (fmt == WMS_FORMAT_3GPP && dl > 0 && dl + 4 <= v.n && dl + 4 <= sizeof(((struct ril_ev_item *)0)->data)) {
            uint8_t buf[sizeof(((struct ril_ev_item *)0)->data)];
            memset(buf, 0, 4); /* no transaction id: nothing to acknowledge */
            memcpy(buf + 4, v.p + 4, dl);
            qmi_logf(QMI_LOG_WARN, "ril: incoming (stored) SMS %u bytes", dl);
            ril_ev_push(r, EV_SMS, 0, buf, dl + 4);
        } else qmi_logf(QMI_LOG_WARN, "ril: stored SMS has unexpected format 0x%02x len %u", fmt, dl);
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

/* ------------------------------------------------------------------ USSD */
/* QmiVoiceUssDataCodingScheme: 1 ASCII, 2 8-bit, 3 UCS2 (UTF-16BE). Layouts per libqmi; ModemManager uses the same. */
static int utf8_next(const uint8_t *s, size_t l, size_t *i, uint32_t *cp)
{
    uint8_t c = s[*i];
    size_t k, j;
    if (c < 0x80) { *cp = c; k = 1; }
    else if ((c & 0xE0) == 0xC0) { *cp = c & 0x1F; k = 2; }
    else if ((c & 0xF0) == 0xE0) { *cp = c & 0x0F; k = 3; }
    else if ((c & 0xF8) == 0xF0) { *cp = c & 0x07; k = 4; }
    else return -EINVAL;
    if (*i + k > l) return -EINVAL;
    for (j = 1; j < k; j++) {
        if ((s[*i + j] & 0xC0) != 0x80) return -EINVAL;
        *cp = (*cp << 6) | (s[*i + j] & 0x3F);
    }
    *i += k;
    return 0;
}

static int ussd_encode(const char *text, uint8_t *dcs, uint8_t *out, size_t cap, size_t *n)
{
    const uint8_t *s = (const uint8_t *)text;
    size_t l = strlen(text), i = 0, o = 0;
    int ascii = 1;
    for (i = 0; i < l; i++)
        if (s[i] >= 0x80) ascii = 0;
    if (ascii) {
        if (l > cap) return -EMSGSIZE;
        memcpy(out, s, l);
        *dcs = 1;
        *n = l;
        return 0;
    }
    i = 0;
    while (i < l) {
        uint32_t cp;
        if (utf8_next(s, l, &i, &cp)) return -EINVAL;
        if (cp >= 0x10000) {
            uint32_t v = cp - 0x10000, hi = 0xD800 | (v >> 10), lo = 0xDC00 | (v & 0x3FF);
            if (o + 4 > cap) return -EMSGSIZE;
            out[o++] = (uint8_t)(hi >> 8); out[o++] = (uint8_t)hi;
            out[o++] = (uint8_t)(lo >> 8); out[o++] = (uint8_t)lo;
        } else {
            if (o + 2 > cap) return -EMSGSIZE;
            out[o++] = (uint8_t)(cp >> 8); out[o++] = (uint8_t)cp;
        }
    }
    *dcs = 3;
    *n = o;
    return 0;
}

static size_t put_utf8(char *o, size_t cap, size_t pos, uint32_t cp)
{
    uint8_t b[4];
    size_t k, j;
    if (cp < 0x80) { b[0] = (uint8_t)cp; k = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); k = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); k = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F); b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); k = 4; }
    if (pos + k + 1 > cap) return pos;
    for (j = 0; j < k; j++) o[pos + j] = (char)b[j];
    return pos + k;
}

/* units: UTF-16 bytes (be = big endian) -> UTF-8 */
static size_t utf16_to_utf8(const uint8_t *d, size_t n, int be, char *out, size_t cap)
{
    size_t i, pos = 0;
    for (i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? ((uint32_t)d[i] << 8 | d[i + 1]) : ((uint32_t)d[i + 1] << 8 | d[i]);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            uint32_t u2 = be ? ((uint32_t)d[i + 2] << 8 | d[i + 3]) : ((uint32_t)d[i + 3] << 8 | d[i + 2]);
            if (u2 >= 0xDC00 && u2 < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00); i += 2; }
        }
        pos = put_utf8(out, cap, pos, u);
    }
    out[pos] = 0;
    return pos;
}

/* TLV with {u8 dcs, u8 len, bytes} (0x12 in responses, 0x10 in the indication) and TLV with UTF-16LE (0x16 / 0x11) */
static void ussd_text(qmi_span_t tlvs, uint8_t uss_id, uint8_t utf16_id, char *out, size_t cap)
{
    qmi_span_t v;
    out[0] = 0;
    if (qmi_tlv_find(tlvs, utf16_id, &v) == 1 && v.n >= 1) {
        size_t cnt = v.p[0];
        if (cnt * 2 + 1 > v.n) cnt = (v.n - 1) / 2;
        if (cnt) { utf16_to_utf8(v.p + 1, cnt * 2, 0, out, cap); return; }
    }
    if (qmi_tlv_find(tlvs, uss_id, &v) == 1 && v.n >= 2) {
        uint8_t dcs = v.p[0];
        size_t len = v.p[1], i, pos = 0;
        if (len + 2 > v.n) len = v.n - 2;
        if (dcs == 3) { utf16_to_utf8(v.p + 2, len, 1, out, cap); return; }
        for (i = 0; i < len; i++) pos = put_utf8(out, cap, pos, v.p[2 + i]);   /* ASCII / 8-bit: bytes as Latin-1 */
        out[pos] = 0;
    }
}

int ril_ussd_send(ril_qmi_t *r, const char *text, char *reply, size_t cap)
{
    uint8_t tl[300], rsp[1100], enc[200], body[210];
    qmi_wbuf_t w;
    qmi_span_t sp;
    size_t n = 0, el = 0;
    uint8_t dcs = 0;
    int rc, answer;
    if (reply && cap) reply[0] = 0;
    if (!text || !*text) return -EINVAL;
    rc = ussd_encode(text, &dcs, enc, sizeof(enc), &el);
    if (rc) return rc;
    body[0] = dcs;
    body[1] = (uint8_t)el;
    memcpy(body + 2, enc, el);
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put(&w, 0x01, body, 2 + el);
    pthread_mutex_lock(&r->mu);
    answer = r->ussd_open;
    r->ussd_open = 0;   /* an indication that arrives while we wait sets it again */
    pthread_mutex_unlock(&r->mu);
    /* never retried: a repeated request would be a second USSD session */
    rc = ril_svcw(r, QMI_SVC_VOICE, answer ? VOICE_ANSWER_USSD : VOICE_ORIG_USSD, &w, 100000, 0, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    if (!answer && reply && cap > 1) {
        sp.p = rsp;
        sp.n = n;
        ussd_text(sp, 0x12, 0x16, reply, cap);
    }
    return 0;
}

int ril_ussd_cancel(ril_qmi_t *r)
{
    int rc = ril_svc(r, QMI_SVC_VOICE, VOICE_CANCEL_USSD, NULL, 0, 15000, 0, NULL, 0, NULL);
    pthread_mutex_lock(&r->mu);
    r->ussd_open = 0;
    pthread_mutex_unlock(&r->mu);
    return rc;
}

void ril_voice_on_indication(struct ril_qmi *r, const qmi_msg_t *m)
{
    if (m->msg_id == VOICE_USSD_IND) {
        char text[1000];
        qmi_span_t v;
        int action = 0;
        ussd_text(m->tlvs, 0x10, 0x11, text, sizeof(text));
        if (qmi_tlv_find(m->tlvs, 0x01, &v) == 1 && v.n >= 1) action = v.p[0];   /* 1 not required, 2 required */
        pthread_mutex_lock(&r->mu);
        r->ussd_open = action == 2;
        pthread_mutex_unlock(&r->mu);
        qmi_logf(QMI_LOG_INFO, "ril: USSD indication, action %d, %zu bytes", action, strlen(text));
        ril_ev_push(r, EV_USSD, action == 2 ? 1 : 0, (const uint8_t *)text, strlen(text));
        return;
    }
    if (m->msg_id == VOICE_USSD_RELEASE_IND) {
        pthread_mutex_lock(&r->mu);
        r->ussd_open = 0;
        pthread_mutex_unlock(&r->mu);
        qmi_logf(QMI_LOG_INFO, "ril: USSD released by the network");
        ril_ev_push(r, EV_USSD, 2, NULL, 0);
        return;
    }
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
    uint8_t tl[12];
    qmi_wbuf_t w;
    int rc;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x10, 1); /* DTMF events; call status indications are expected without registration [UNVERIFIED] */
    qmi_tlv_put_u8(&w, 0x16, 1); /* USSD notification events (libqmi: Indication Register, TLV 0x16) */
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
    uint8_t st;
    if (m->msg_id != WDS_PKT_STATUS_IND) return;
    /* Connection status (libqmi QmiWdsConnectionStatus): 0 unknown, 1 disconnected, 2 connected, 3 suspended,
     * 4 authenticating. Only a real disconnect may tear the Android network down: 3/4 happen while the call is still
     * alive (dormancy, re-auth) and treating them as "down" made Android drop a working session. */
    if (qmi_tlv_find(m->tlvs, 0x01, &v) != 1 || v.n < 1) return;
    st = v.p[0];
    qmi_logf(QMI_LOG_INFO, "ril: WDS packet service status %u", st);
    if (st == 1) ril_ev_push(r, EV_DATA, 0, NULL, 0);
    else if (st == 2) ril_ev_push(r, EV_DATA, 1, NULL, 0);
}

static void ip_from_wire(uint8_t out[4], const uint8_t *wire) /* wire is the little endian u32 of the host-order address */
{
    out[0] = wire[3];
    out[1] = wire[2];
    out[2] = wire[1];
    out[3] = wire[0];
}

static unsigned ipv4_prefix(const uint8_t mask[4])
{
    unsigned i, b, n = 0;
    for (i = 0; i < 4; i++)
        for (b = 0; b < 8; b++) n += (mask[i] >> b) & 1;
    return n;
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

/* Idempotent: only the settings that really differ are written, so an interface that is already in the requested state
 * needs no privileges (and the host tests can use "lo" as a non-root user). */
static int iface_set(const char *ifname, int up, int mtu)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), rc = 0;
    if (s < 0) return -errno;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (mtu > 0) {
        int cur = -1;
        if (ioctl(s, SIOCGIFMTU, &ifr) == 0) cur = ifr.ifr_mtu;
        if (cur != mtu) {
            ifr.ifr_mtu = mtu;
            if (ioctl(s, SIOCSIFMTU, &ifr) < 0) rc = -errno;
        }
    }
    memset(&ifr.ifr_ifru, 0, sizeof(ifr.ifr_ifru));
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) {
        rc = -errno;
    } else {
        short want = ifr.ifr_flags;
        if (up) want |= IFF_UP;
        else want &= ~IFF_UP;
        if (want != ifr.ifr_flags) {
            ifr.ifr_flags = want;
            if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) rc = -errno;
        }
    }
    close(s);
    return rc;
}

/* The IPv4 address of a cellular interface is configured by the RIL, not by the framework: ConnectivityService only
 * installs the interface, routes and DNS (updateLinkProperties has no address step), so a HAL that merely reports the
 * address leaves the link without one and nothing can be sent (seen on the real device: netd adds routes and DNS but
 * "ip addr show wwan0" has no inet line, validation fails, and adding the address by hand makes the network work at
 * once). Legacy vendor RILs do the same (reference-ril: "netcfg rmnet0 dhcp"). SIOCSIFADDR/SIOCSIFNETMASK do not
 * disturb the routes that netd installs. */
static int iface_ipv4_set(const char *ifname, const uint8_t ip[4], const uint8_t mask[4])
{
    struct ifreq ifr;
    struct sockaddr_in *sin;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), rc = 0;
    if (s < 0) return -errno;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    /* Never touch a loopback: that is the host-test interface, and replacing 127.0.0.1 would break the test itself. */
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_LOOPBACK)) {
        close(s);
        return 0;
    }
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    memcpy(&sin->sin_addr, ip, 4);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) rc = -errno;
    if (!rc) {
        sin = (struct sockaddr_in *)&ifr.ifr_netmask;
        sin->sin_family = AF_INET;
        memcpy(&sin->sin_addr, mask, 4);
        if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0) rc = -errno;
    }
    close(s);
    return rc;
}

/* Removes the address that this RIL put on the link when the data call ends, so a reconnect with a different IP does
 * not leave a stale address behind (Android tears the network down but never removes the IFA for cellular). */
static int iface_ipv4_del(const char *ifname)
{
    struct ifreq ifr, del;
    struct sockaddr_in *sin;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), rc = 0;
    if (s < 0) return -errno;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0 && (ifr.ifr_flags & IFF_LOOPBACK)) {
        close(s);
        return 0;
    }
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        sin = (struct sockaddr_in *)&ifr.ifr_addr;
        if (sin->sin_family == AF_INET && sin->sin_addr.s_addr != 0) {
            memset(&del, 0, sizeof(del));
            snprintf(del.ifr_name, sizeof(del.ifr_name), "%s", ifname);
            del.ifr_addr = ifr.ifr_addr;
            if (ioctl(s, SIOCDIFADDR, &del) < 0) rc = -errno;
        }
    }
    close(s);
    return rc;
}

/* The BAM-DMUX netdev of the kernel carries raw IP, while the modem reported link layer 802.3 for its default endpoint.
 * A modem that expects Ethernet frames drops raw IP packets (seen: WDS packet counters stay 0 although the kernel
 * transmits). Set raw IP (2) for the DMUX channel's endpoint and read it back. Layouts per libqmi/ModemManager (not
 * verified on this modem): Set Data Format 0x0020: 0x10 qos(u8), 0x11 link layer(u32), 0x12 UL aggr(u32), 0x13 DL aggr(u32),
 * 0x17 endpoint {type u32, iface u32}; Get Data Format 0x0021: 0x10 endpoint. Failures are reported, not fatal. */
/* Experiment switches without a rebuild: /data/local/tmp/j5ril-state/data.conf, lines "key=value".
 *   wda=on|off     set raw IP on the DMUX endpoint before the call (default on)
 *   bind=sio|none|mux1|auto   sio (default): WDS Bind Data Port 0x0089 with SIO port A2_MUX_RMNETn (BAM-DMUX channel n);
 *                         none; mux1 / auto: WDS Bind Mux Data Port 0x00A2 variants (QMAP style; the modem answered 0x30) */
static void data_conf(const char *key, const char *def, char *out, size_t cap)
{
    char line[96];
    size_t kl = strlen(key);
    FILE *f = fopen("/data/local/tmp/j5ril-state/data.conf", "r");
    snprintf(out, cap, "%s", def);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, kl) == 0 && line[kl] == '=') {
            size_t i = 0;
            snprintf(out, cap, "%s", line + kl + 1);
            while (out[i] && out[i] != '\n' && out[i] != '\r' && out[i] != ' ') i++;
            out[i] = 0;
        }
    }
    fclose(f);
}

static void wda_set_raw_ip(struct ril_qmi *r, long port)
{
    uint8_t tl[64], rsp[128], ep[8];
    qmi_wbuf_t w;
    qmi_span_t sp, v;
    size_t n = 0;
    int rc;
    if (!r->cid[QMI_SVC_WDA]) { qmi_logf(QMI_LOG_INFO, "ril: no WDA client, data format not set"); return; }
    ep[0] = QMI_DATA_ENDPOINT_BAM_DMUX; ep[1] = ep[2] = ep[3] = 0;
    ep[4] = (uint8_t)port; ep[5] = (uint8_t)(port >> 8); ep[6] = ep[7] = 0;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x10, 0);
    qmi_tlv_put_u32(&w, 0x11, 2);
    qmi_tlv_put_u32(&w, 0x12, 0);
    qmi_tlv_put_u32(&w, 0x13, 0);
    qmi_tlv_put(&w, 0x17, ep, sizeof(ep));
    rc = ril_svcw(r, QMI_SVC_WDA, WDA_SET_DATA_FORMAT, &w, 0, 0, rsp, sizeof(rsp), &n);
    qmi_logf(QMI_LOG_WARN, "ril: WDA set data format raw IP (BAM_DMUX channel %ld) -> %d", port, rc);
    fprintf(stderr, "ril: WDA set data format raw IP (BAM_DMUX channel %ld) -> %d\n", port, rc);
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put(&w, 0x10, ep, sizeof(ep));
    rc = ril_svcw(r, QMI_SVC_WDA, WDA_GET_DATA_FORMAT, &w, 0, 0, rsp, sizeof(rsp), &n);
    if (rc) {
        qmi_logf(QMI_LOG_WARN, "ril: WDA get data format for the endpoint failed (%d)", rc);
        fprintf(stderr, "ril: WDA get data format for the endpoint failed (%d)\n", rc);
        return;
    }
    sp.p = rsp; sp.n = n;
    if (qmi_tlv_find(sp, 0x11, &v) == 1 && v.n >= 4) {
        qmi_logf(QMI_LOG_WARN, "ril: WDA link layer protocol of the DMUX endpoint = %u (1 = 802.3, 2 = raw IP)", le32(v.p));
        fprintf(stderr, "ril: WDA link layer protocol of the DMUX endpoint = %u (1 = 802.3, 2 = raw IP)\n", le32(v.p));
    }
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
        char c_wda[16], c_bind[16];
        data_conf("wda", "on", c_wda, sizeof(c_wda));
        data_conf("bind", "sio", c_bind, sizeof(c_bind));
        qmi_logf(QMI_LOG_WARN, "ril: data setup knobs: wda=%s bind=%s (data.conf)", c_wda, c_bind);
        fprintf(stderr, "ril: data setup knobs: wda=%s bind=%s (data.conf)\n", c_wda, c_bind);
        if (strcmp(c_wda, "off") != 0) wda_set_raw_ip(r, port);
        if (strcmp(c_bind, "sio") == 0) {
            /* BAM-DMUX (a2 mux) has no QMAP: the WDS client is tied to the channel through its SIO port.
             * Bind Data Port 0x0089, TLV 0x01 = u16 QmiSioPort (libqmi data/qmi-service-wds.json). Not verified here. */
            qmi_wbuf_init(&w, tl, sizeof(tl));
            qmi_tlv_put_u16(&w, 0x01, (uint16_t)(SIO_PORT_A2_MUX_RMNET0 + port));
            rc = ril_svcw(r, QMI_SVC_WDS, WDS_BIND_DATA_PORT, &w, 0, 0, NULL, 0, NULL);
            fprintf(stderr, "ril: WDS bind data port (SIO 0x%04lx, channel %ld) -> %d\n", (long)SIO_PORT_A2_MUX_RMNET0 + port, port, rc);
            qmi_logf(QMI_LOG_WARN, "ril: WDS bind data port (SIO 0x%04lx, channel %ld) -> %d", (long)SIO_PORT_A2_MUX_RMNET0 + port, port, rc);
        } else if (strcmp(c_bind, "none") != 0) {
            /* The modem answered 0x30 to the endpoint-only request; mux id / client type are optional in the IDL, but
             * try them in turn (all variants are harmless: they only configure this WDS client). The first success wins. */
            struct bindvar { int mux; int client; };
            static const struct bindvar var_all[] = {
                {-1, -1}, {0, -1}, {1, -1}, {0xFF, -1}, {-1, 1}, {1, 1}, {0xFF, 0xFF},
            };
            static const struct bindvar var_mux1[] = { {1, -1} };
            const int one = strcmp(c_bind, "mux1") == 0;
            const struct bindvar *var = one ? var_mux1 : var_all;
            const size_t nvar = one ? 1 : sizeof(var_all) / sizeof(var_all[0]);
            uint8_t ep[8];
            size_t vi;
            ep[0] = QMI_DATA_ENDPOINT_BAM_DMUX; ep[1] = ep[2] = ep[3] = 0;
            ep[4] = (uint8_t)port; ep[5] = (uint8_t)(port >> 8); ep[6] = ep[7] = 0;
            for (vi = 0; vi < nvar; vi++) {
                qmi_wbuf_init(&w, tl, sizeof(tl));
                qmi_tlv_put(&w, 0x10, ep, sizeof(ep));
                if (var[vi].mux >= 0) qmi_tlv_put_u8(&w, 0x11, (uint8_t)var[vi].mux);
                if (var[vi].client >= 0) qmi_tlv_put_u32(&w, 0x13, (uint32_t)var[vi].client);
                rc = ril_svcw(r, QMI_SVC_WDS, WDS_BIND_MUX_DATA_PORT, &w, 0, 0, NULL, 0, NULL);
                fprintf(stderr, "ril: WDS bind mux data port (BAM_DMUX, channel %ld, mux %d, client %d) -> %d\n",
                        port, var[vi].mux, var[vi].client, rc);
                qmi_logf(QMI_LOG_WARN, "ril: WDS bind mux data port (channel %ld, mux %d, client %d) -> %d", port, var[vi].mux, var[vi].client, rc);
                if (rc == 0) break;
            }
        }
    }

    /* BAM-DMUX: opening the netdev sends the OPEN command for the channel to the modem (qcom_bam_dmux, ndo_open). Do it
     * BEFORE the data call starts, like the downstream stack does; previously the link came up only after Start Network. */
    rc = iface_set(out->ifname, 1, 0);
    qmi_logf(QMI_LOG_WARN, "ril: %s brought up before Start Network -> %d", out->ifname, rc);
    fprintf(stderr, "ril: %s brought up before Start Network -> %d\n", out->ifname, rc);

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
    if (rc) {
        (void)iface_set(out->ifname, 0, 0);
        return rc;
    }
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
    rc = iface_set(out->ifname, 1, out->mtu);
    if (rc) {
        qmi_logf(QMI_LOG_ERROR, "ril: cannot bring %s up: %d", out->ifname, rc);
        (void)ril_data_stop(r, out->handle, NULL);
        memset(out, 0, sizeof(*out));
        return rc;
    }
    /* The framework installs the interface, routes and DNS but never the IPv4 address (only the vendor RIL does that),
     * so configure it here. No mask from the modem: a /32 still works because netd installs the on-link route itself. */
    if (!out->mask[0] && !out->mask[1] && !out->mask[2] && !out->mask[3])
        memset(out->mask, 0xff, 4);
    rc = iface_ipv4_set(out->ifname, out->ip, out->mask);
    qmi_logf(rc ? QMI_LOG_ERROR : QMI_LOG_WARN, "ril: %s IPv4 %u.%u.%u.%u/%u configured -> %d", out->ifname,
             out->ip[0], out->ip[1], out->ip[2], out->ip[3], ipv4_prefix(out->mask), rc);
    fprintf(stderr, "ril: %s IPv4 %u.%u.%u.%u/%u configured -> %d\n", out->ifname,
            out->ip[0], out->ip[1], out->ip[2], out->ip[3], ipv4_prefix(out->mask), rc);
    /* Not fatal: the call itself is up, and a host build/test or a HAL without NET_ADMIN may not be allowed to do it. */
    if (rc) qmi_logf(QMI_LOG_ERROR, "ril: cannot configure the IPv4 address of %s: %d", out->ifname, rc);
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
    if (ifname && *ifname) {
        (void)iface_ipv4_del(ifname);
        (void)iface_set(ifname, 0, 0);
    }
    /* an already dead session (invalid handle / out of call) counts as stopped */
    if (QMI_E_IS_QMI(rc) && (QMI_E_CODE(rc) == QMI_ERR_INVALID_HANDLE || QMI_E_CODE(rc) == QMI_ERR_OUT_OF_CALL)) return 0;
    return rc;
}

/* WDS packet statistics straight from the modem (diagnostics). The response TLVs are returned raw; the caller prints them.
 * The BAM-DMUX netdev of the kernel keeps no counters at all, so this is the only way to see whether the modem got packets. */
/* One line into the log: kernel counters of the netdev next to the modem's own WDS counters. If the kernel transmits and
 * the modem counts nothing, the packets die between BAM-DMUX and the modem; if the modem counts TX but the kernel gets no
 * RX, the answer is lost in the other direction. */
void ril_data_diag(ril_qmi_t *r, const char *ifname, const char *tag)
{
    static const char *const names[] = {"rx_packets", "tx_packets", "rx_dropped", "tx_dropped", "tx_errors", "rx_errors"};
    char line[512], pth[128], buf[32];
    uint8_t st[256];
    size_t n = 0, i, o = 0;
    int rc;
    o += (size_t)snprintf(line + o, sizeof(line) - o, "ril: data diag %s %s kernel:", tag, ifname);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        FILE *f;
        buf[0] = 0;
        snprintf(pth, sizeof(pth), "/sys/class/net/%s/statistics/%s", ifname, names[i]);
        f = fopen(pth, "r");
        if (f) { if (!fgets(buf, sizeof(buf), f)) buf[0] = 0; fclose(f); }
        buf[strcspn(buf, "\n")] = 0;
        o += (size_t)snprintf(line + o, sizeof(line) - o, " %s=%s", names[i], buf[0] ? buf : "?");
    }
    rc = ril_data_pkt_stats(r, st, sizeof(st), &n);
    o += (size_t)snprintf(line + o, sizeof(line) - o, " | modem WDS stats rc=%d:", rc);
    for (i = 0; rc == 0 && i < n && i < 96 && o + 4 < sizeof(line); i++) o += (size_t)snprintf(line + o, sizeof(line) - o, "%02x", st[i]);
    qmi_logf(QMI_LOG_WARN, "%s", line);
    fprintf(stderr, "%s\n", line);
}

int ril_data_pkt_stats(ril_qmi_t *r, uint8_t *out, size_t cap, size_t *outlen)
{
    uint8_t tl[16];
    qmi_wbuf_t w;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u32(&w, 0x01, 0xFF); /* all counters (mask bits per libqmi, unverified) */
    return ril_svcw(r, QMI_SVC_WDS, WDS_GET_PKT_STATISTICS, &w, 0, 0, out, cap, outlen);
}
