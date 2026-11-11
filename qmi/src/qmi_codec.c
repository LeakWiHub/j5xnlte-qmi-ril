/* SPDX-License-Identifier: Apache-2.0 */
/* Pure QMUX/QMI/TLV codec. No I/O, no malloc, no stdio: this file is also
 * linked into the freestanding qmi-probe binary. */
#include "qmi/qmi_codec.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* names                                                               */
/* ------------------------------------------------------------------ */
const char *qmi_service_name(uint8_t s)
{
    switch (s) {
    case QMI_SVC_CTL: return "CTL";
    case QMI_SVC_WDS: return "WDS";
    case QMI_SVC_DMS: return "DMS";
    case QMI_SVC_NAS: return "NAS";
    case QMI_SVC_QOS: return "QOS";
    case QMI_SVC_WMS: return "WMS";
    case QMI_SVC_PDS: return "PDS";
    case QMI_SVC_AUTH: return "AUTH";
    case QMI_SVC_AT: return "AT";
    case QMI_SVC_VOICE: return "VOICE";
    case QMI_SVC_CAT2: return "CAT2";
    case QMI_SVC_UIM: return "UIM";
    case QMI_SVC_PBM: return "PBM";
    case QMI_SVC_LOC: return "LOC";
    case QMI_SVC_SAR: return "SAR";
    case QMI_SVC_WDA: return "WDA";
    case QMI_SVC_COEX: return "COEX";
    case QMI_SVC_PDC: return "PDC";
    case QMI_SVC_DSD: return "DSD";
    default: return "?";
    }
}

const char *qmi_error_str(uint16_t e)
{
    switch (e) {
    case QMI_ERR_NONE: return "NONE";
    case QMI_ERR_MALFORMED_MSG: return "MALFORMED_MSG";
    case QMI_ERR_NO_MEMORY: return "NO_MEMORY";
    case QMI_ERR_INTERNAL: return "INTERNAL";
    case QMI_ERR_ABORTED: return "ABORTED";
    case QMI_ERR_CLIENT_IDS_EXHAUSTED: return "CLIENT_IDS_EXHAUSTED";
    case QMI_ERR_UNABORTABLE_TRANSACTION: return "UNABORTABLE_TRANSACTION";
    case QMI_ERR_INVALID_CLIENT_ID: return "INVALID_CLIENT_ID";
    case QMI_ERR_NO_THRESHOLDS: return "NO_THRESHOLDS";
    case QMI_ERR_INVALID_HANDLE: return "INVALID_HANDLE";
    case QMI_ERR_INVALID_PROFILE: return "INVALID_PROFILE";
    case QMI_ERR_INVALID_PINID: return "INVALID_PINID";
    case QMI_ERR_INCORRECT_PIN: return "INCORRECT_PIN";
    case QMI_ERR_NO_NETWORK_FOUND: return "NO_NETWORK_FOUND";
    case QMI_ERR_CALL_FAILED: return "CALL_FAILED";
    case QMI_ERR_OUT_OF_CALL: return "OUT_OF_CALL";
    case QMI_ERR_NOT_PROVISIONED: return "NOT_PROVISIONED";
    case QMI_ERR_MISSING_ARG: return "MISSING_ARG";
    case QMI_ERR_ARG_TOO_LONG: return "ARG_TOO_LONG";
    case QMI_ERR_INVALID_TX_ID: return "INVALID_TX_ID";
    case QMI_ERR_DEVICE_IN_USE: return "DEVICE_IN_USE";
    case QMI_ERR_OP_NETWORK_UNSUPPORTED: return "OP_NETWORK_UNSUPPORTED";
    case QMI_ERR_OP_DEVICE_UNSUPPORTED: return "OP_DEVICE_UNSUPPORTED";
    case QMI_ERR_NO_EFFECT: return "NO_EFFECT";
    case QMI_ERR_NO_FREE_PROFILE: return "NO_FREE_PROFILE";
    case QMI_ERR_INVALID_PDP_TYPE: return "INVALID_PDP_TYPE";
    case QMI_ERR_INVALID_TECH_PREF: return "INVALID_TECH_PREF";
    case QMI_ERR_INVALID_PROFILE_TYPE: return "INVALID_PROFILE_TYPE";
    case QMI_ERR_INVALID_SERVICE_TYPE: return "INVALID_SERVICE_TYPE";
    case QMI_ERR_INVALID_REGISTER_ACTION: return "INVALID_REGISTER_ACTION";
    case QMI_ERR_INVALID_PS_ATTACH_ACTION: return "INVALID_PS_ATTACH_ACTION";
    case QMI_ERR_AUTHENTICATION_FAILED: return "AUTHENTICATION_FAILED";
    case QMI_ERR_PIN_BLOCKED: return "PIN_BLOCKED";
    case QMI_ERR_PIN_PERM_BLOCKED: return "PIN_PERM_BLOCKED";
    case QMI_ERR_UIM_NOT_INITIALIZED: return "UIM_NOT_INITIALIZED";
    case QMI_ERR_INVALID_QMI_CMD: return "INVALID_QMI_CMD";
    case QMI_ERR_INFO_UNAVAILABLE: return "INFO_UNAVAILABLE";
    case QMI_ERR_ACCESS_DENIED: return "ACCESS_DENIED";
    case QMI_ERR_HARDWARE_RESTRICTED: return "HARDWARE_RESTRICTED";
    default: return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* reader                                                              */
/* ------------------------------------------------------------------ */
void qmi_rd_init(qmi_rd_t *r, const uint8_t *p, size_t n)
{
    r->p = p;
    r->n = n;
    r->off = 0;
    r->err = 0;
}

static int rd_need(qmi_rd_t *r, size_t k)
{
    if (r->err || r->n - r->off < k) {
        r->err = 1;
        return 0;
    }
    return 1;
}

uint8_t qmi_rd_u8(qmi_rd_t *r)
{
    if (!rd_need(r, 1)) return 0;
    return r->p[r->off++];
}

uint16_t qmi_rd_u16(qmi_rd_t *r)
{
    uint16_t v;
    if (!rd_need(r, 2)) return 0;
    v = (uint16_t)(r->p[r->off] | (r->p[r->off + 1] << 8));
    r->off += 2;
    return v;
}

uint32_t qmi_rd_u32(qmi_rd_t *r)
{
    uint32_t v;
    if (!rd_need(r, 4)) return 0;
    v = (uint32_t)r->p[r->off] | ((uint32_t)r->p[r->off + 1] << 8) |
        ((uint32_t)r->p[r->off + 2] << 16) | ((uint32_t)r->p[r->off + 3] << 24);
    r->off += 4;
    return v;
}

uint64_t qmi_rd_u64(qmi_rd_t *r)
{
    uint64_t lo = qmi_rd_u32(r);
    uint64_t hi = qmi_rd_u32(r);
    return lo | (hi << 32);
}

int8_t qmi_rd_i8(qmi_rd_t *r) { return (int8_t)qmi_rd_u8(r); }
int16_t qmi_rd_i16(qmi_rd_t *r) { return (int16_t)qmi_rd_u16(r); }
int32_t qmi_rd_i32(qmi_rd_t *r) { return (int32_t)qmi_rd_u32(r); }

const uint8_t *qmi_rd_bytes(qmi_rd_t *r, size_t n)
{
    const uint8_t *p;
    if (!rd_need(r, n)) return NULL;
    p = r->p + r->off;
    r->off += n;
    return p;
}

size_t qmi_rd_remaining(const qmi_rd_t *r) { return r->err ? 0 : r->n - r->off; }
int qmi_rd_ok(const qmi_rd_t *r) { return !r->err; }

size_t qmi_copy_str(char *dst, size_t cap, const uint8_t *src, size_t srclen)
{
    size_t n;
    if (cap == 0) return 0;
    n = srclen < cap - 1 ? srclen : cap - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ */
/* writer                                                              */
/* ------------------------------------------------------------------ */
void qmi_wbuf_init(qmi_wbuf_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
}

int qmi_wbuf_ok(const qmi_wbuf_t *w) { return !w->overflow; }

static int wb_need(qmi_wbuf_t *w, size_t k)
{
    if (w->overflow || w->cap - w->len < k) {
        w->overflow = 1;
        return 0;
    }
    return 1;
}

int qmi_wbuf_u8(qmi_wbuf_t *w, uint8_t v)
{
    if (!wb_need(w, 1)) return -1;
    w->buf[w->len++] = v;
    return 0;
}

int qmi_wbuf_u16(qmi_wbuf_t *w, uint16_t v)
{
    if (!wb_need(w, 2)) return -1;
    w->buf[w->len++] = (uint8_t)v;
    w->buf[w->len++] = (uint8_t)(v >> 8);
    return 0;
}

int qmi_wbuf_u32(qmi_wbuf_t *w, uint32_t v)
{
    if (!wb_need(w, 4)) return -1;
    w->buf[w->len++] = (uint8_t)v;
    w->buf[w->len++] = (uint8_t)(v >> 8);
    w->buf[w->len++] = (uint8_t)(v >> 16);
    w->buf[w->len++] = (uint8_t)(v >> 24);
    return 0;
}

int qmi_wbuf_u64(qmi_wbuf_t *w, uint64_t v)
{
    if (!wb_need(w, 8)) return -1;
    qmi_wbuf_u32(w, (uint32_t)v);
    qmi_wbuf_u32(w, (uint32_t)(v >> 32));
    return 0;
}

int qmi_wbuf_bytes(qmi_wbuf_t *w, const void *p, size_t n)
{
    if (!wb_need(w, n)) return -1;
    if (n) memcpy(w->buf + w->len, p, n);
    w->len += n;
    return 0;
}

int qmi_tlv_put(qmi_wbuf_t *w, uint8_t type, const void *v, size_t len)
{
    if (len > 0xFFFF) {
        w->overflow = 1;
        return -1;
    }
    if (!wb_need(w, 3 + len)) return -1;
    qmi_wbuf_u8(w, type);
    qmi_wbuf_u16(w, (uint16_t)len);
    qmi_wbuf_bytes(w, v, len);
    return 0;
}

int qmi_tlv_put_u8(qmi_wbuf_t *w, uint8_t type, uint8_t v) { return qmi_tlv_put(w, type, &v, 1); }

int qmi_tlv_put_u16(qmi_wbuf_t *w, uint8_t type, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    return qmi_tlv_put(w, type, b, 2);
}

int qmi_tlv_put_u32(qmi_wbuf_t *w, uint8_t type, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return qmi_tlv_put(w, type, b, 4);
}

int qmi_tlv_put_u64(qmi_wbuf_t *w, uint8_t type, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    return qmi_tlv_put(w, type, b, 8);
}

int qmi_tlv_put_str(qmi_wbuf_t *w, uint8_t type, const char *s)
{
    return qmi_tlv_put(w, type, s, strlen(s));
}

int qmi_tlv_begin(qmi_wbuf_t *w, uint8_t type, size_t *mark)
{
    if (!wb_need(w, 3)) return -1;
    *mark = w->len;
    qmi_wbuf_u8(w, type);
    qmi_wbuf_u16(w, 0);
    return 0;
}

int qmi_tlv_end(qmi_wbuf_t *w, size_t mark)
{
    size_t body;
    if (w->overflow) return -1;
    body = w->len - (mark + 3);
    if (body > 0xFFFF) {
        w->overflow = 1;
        return -1;
    }
    w->buf[mark + 1] = (uint8_t)body;
    w->buf[mark + 2] = (uint8_t)(body >> 8);
    return 0;
}

/* ------------------------------------------------------------------ */
/* TLV navigation                                                      */
/* ------------------------------------------------------------------ */
int qmi_tlv_next(qmi_span_t t, size_t *off, uint8_t *type, qmi_span_t *value)
{
    size_t o = *off, len;
    if (o == t.n) return 0;
    if (t.n - o < 3) return -1;
    len = (size_t)t.p[o + 1] | ((size_t)t.p[o + 2] << 8);
    if (t.n - o - 3 < len) return -1;
    *type = t.p[o];
    value->p = t.p + o + 3;
    value->n = len;
    *off = o + 3 + len;
    return 1;
}

int qmi_tlv_find(qmi_span_t t, uint8_t type, qmi_span_t *value)
{
    size_t off = 0;
    uint8_t ty;
    qmi_span_t v;
    int rc;
    while ((rc = qmi_tlv_next(t, &off, &ty, &v)) > 0) {
        if (ty == type) {
            if (value) *value = v;
            return 1;
        }
    }
    return rc; /* 0 absent, -1 malformed */
}

int qmi_tlv_validate(qmi_span_t t)
{
    size_t off = 0;
    uint8_t ty;
    qmi_span_t v;
    int rc;
    while ((rc = qmi_tlv_next(t, &off, &ty, &v)) > 0) {
    }
    return rc < 0 ? -1 : 0;
}

int qmi_resp_parse(qmi_span_t tlvs, qmi_resp_t *out)
{
    qmi_span_t v;
    int rc;
    memset(out, 0, sizeof(*out));
    out->tlvs = tlvs;
    rc = qmi_tlv_find(tlvs, QMI_TLV_RESULT, &v);
    if (rc < 0) return -1;
    if (rc == 1) {
        qmi_rd_t r;
        qmi_rd_init(&r, v.p, v.n);
        out->result = qmi_rd_u16(&r);
        out->error = qmi_rd_u16(&r);
        if (!qmi_rd_ok(&r)) return -1;
        out->has_result = 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* QMUX frames                                                         */
/* ------------------------------------------------------------------ */
#define QMUX_HDR 6      /* marker + len(2) + flags + service + client */
#define CTL_SDU_HDR 6   /* flags, txid(1), msg(2), tlvlen(2) */
#define SVC_SDU_HDR 7   /* flags, txid(2), msg(2), tlvlen(2) */

static uint8_t qmi_flags_for(uint8_t service, qmi_msg_type_t t)
{
    if (service == QMI_SVC_CTL) return t == QMI_MSG_RESPONSE ? 0x01 : t == QMI_MSG_INDICATION ? 0x02 : 0x00;
    return t == QMI_MSG_RESPONSE ? 0x02 : t == QMI_MSG_INDICATION ? 0x04 : 0x00;
}

size_t qmi_msg_encode(uint8_t *out, size_t cap, uint8_t service, uint8_t client_id,
                      qmi_msg_type_t type, uint16_t txid, uint16_t msg_id,
                      const uint8_t *tlvs, size_t tlv_len)
{
    size_t sdu = service == QMI_SVC_CTL ? CTL_SDU_HDR : SVC_SDU_HDR;
    size_t total = QMUX_HDR + sdu + tlv_len;
    size_t i = 0;
    if (tlv_len > 0xFFFF || total - 1 > 0xFFFF || total > cap) return 0;
    out[i++] = QMI_QMUX_MARKER;
    out[i++] = (uint8_t)(total - 1);
    out[i++] = (uint8_t)((total - 1) >> 8);
    out[i++] = type == QMI_MSG_REQUEST ? 0x00 : 0x80;
    out[i++] = service;
    out[i++] = client_id;
    out[i++] = qmi_flags_for(service, type);
    out[i++] = (uint8_t)txid;
    if (service != QMI_SVC_CTL) out[i++] = (uint8_t)(txid >> 8);
    out[i++] = (uint8_t)msg_id;
    out[i++] = (uint8_t)(msg_id >> 8);
    out[i++] = (uint8_t)tlv_len;
    out[i++] = (uint8_t)(tlv_len >> 8);
    if (tlv_len) memcpy(out + i, tlvs, tlv_len);
    return total;
}

int qmi_msg_decode(const uint8_t *f, size_t len, qmi_msg_t *m)
{
    size_t sdu, i, tlv_len, qmux_len;
    if (len < QMUX_HDR + CTL_SDU_HDR) return QMI_DEC_SHORT;
    if (f[0] != QMI_QMUX_MARKER) return QMI_DEC_MARKER;
    qmux_len = (size_t)f[1] | ((size_t)f[2] << 8);
    if (qmux_len + 1 != len) return QMI_DEC_LENGTH;
    memset(m, 0, sizeof(*m));
    m->qmux_flags = f[3];
    m->service = f[4];
    m->client_id = f[5];
    sdu = m->service == QMI_SVC_CTL ? CTL_SDU_HDR : SVC_SDU_HDR;
    if (len < QMUX_HDR + sdu) return QMI_DEC_SHORT;
    i = QMUX_HDR;
    m->qmi_flags = f[i++];
    m->txid = f[i++];
    if (m->service != QMI_SVC_CTL) m->txid = (uint16_t)(m->txid | (f[i++] << 8));
    m->msg_id = (uint16_t)(f[i] | (f[i + 1] << 8));
    i += 2;
    tlv_len = (size_t)f[i] | ((size_t)f[i + 1] << 8);
    i += 2;
    if (tlv_len > len - i) return QMI_DEC_TLVLEN;
    if (m->service == QMI_SVC_CTL) {
        m->type = (m->qmi_flags & 0x01) ? QMI_MSG_RESPONSE : (m->qmi_flags & 0x02) ? QMI_MSG_INDICATION : QMI_MSG_REQUEST;
    } else {
        m->type = (m->qmi_flags & 0x02) ? QMI_MSG_RESPONSE : (m->qmi_flags & 0x04) ? QMI_MSG_INDICATION : QMI_MSG_REQUEST;
    }
    m->tlvs.p = f + i;
    m->tlvs.n = tlv_len;
    return 0;
}

/* ------------------------------------------------------------------ */
/* de-framer                                                           */
/* ------------------------------------------------------------------ */
void qmi_deframer_init(qmi_deframer_t *d) { memset(d, 0, sizeof(*d)); }

static void df_flush(qmi_deframer_t *d)
{
    if (d->consume) {
        if (d->consume >= d->len) {
            d->len = 0;
        } else {
            memmove(d->buf, d->buf + d->consume, d->len - d->consume);
            d->len -= d->consume;
        }
        d->consume = 0;
    }
}

static void df_drop(qmi_deframer_t *d, size_t n)
{
    if (n > d->len) n = d->len;
    memmove(d->buf, d->buf + n, d->len - n);
    d->len -= n;
    d->dropped_bytes += n;
}

size_t qmi_deframer_push(qmi_deframer_t *d, const uint8_t *data, size_t n)
{
    size_t room, take;
    df_flush(d);
    room = QMI_MAX_FRAME + 16 - d->len;
    take = n < room ? n : room;
    if (take) memcpy(d->buf + d->len, data, take);
    d->len += take;
    return take;
}

int qmi_deframer_pop(qmi_deframer_t *d, qmi_msg_t *msg)
{
    df_flush(d);
    for (;;) {
        size_t i, qlen, total;
        int rc;
        /* resync on the marker byte */
        for (i = 0; i < d->len && d->buf[i] != QMI_QMUX_MARKER; i++) {
        }
        if (i) {
            df_drop(d, i);
            d->resyncs++;
        }
        if (d->len < 3) return 0;
        qlen = (size_t)d->buf[1] | ((size_t)d->buf[2] << 8);
        total = qlen + 1;
        /* shortest legal frame is a CTL message without TLVs (12 bytes) */
        if (total < QMUX_HDR + CTL_SDU_HDR || total > QMI_MAX_FRAME) {
            df_drop(d, 1); /* not a real header: skip the marker and rescan */
            d->resyncs++;
            continue;
        }
        if (d->len < total) return 0;
        rc = qmi_msg_decode(d->buf, total, msg);
        if (rc == 0) {
            d->consume = total;
            return 1;
        }
        df_drop(d, 1); /* looked like a frame but is not: rescan after marker */
        d->resyncs++;
    }
}

/* ------------------------------------------------------------------ */
/* hex                                                                 */
/* ------------------------------------------------------------------ */
void qmi_hex(char *dst, size_t cap, const uint8_t *p, size_t n)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t i, o = 0;
    if (cap == 0) return;
    for (i = 0; i < n && o + 3 < cap; i++) {
        if (i) dst[o++] = ':';
        dst[o++] = hx[p[i] >> 4];
        dst[o++] = hx[p[i] & 15];
    }
    dst[o] = '\0';
}
