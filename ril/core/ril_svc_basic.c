/* SPDX-License-Identifier: Apache-2.0
 * ril_svc_basic.c - DMS (modem identity/power), UIM (SIM) and NAS (network). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ril_internal.h"

#define DMS_GET_REVISION 0x0023
#define DMS_GET_IDS 0x0025
#define DMS_GET_OPMODE 0x002D
#define DMS_SET_OPMODE 0x002E
#define DMS_UIM_GET_ICCID 0x003C
#define DMS_UIM_GET_IMSI 0x0043

#define UIM_READ_TRANSPARENT 0x0020
#define UIM_READ_RECORD 0x0021
#define UIM_GET_FILE_ATTRS 0x0024
#define UIM_SET_PIN_PROTECTION 0x0025
#define UIM_VERIFY_PIN 0x0026
#define UIM_UNBLOCK_PIN 0x0027
#define UIM_CHANGE_PIN 0x0028
#define UIM_EVENT_REG 0x002E
#define UIM_CHANGE_PROVISIONING_SESSION 0x0038
#define UIM_GET_CARD_STATUS 0x002F
#define UIM_STATUS_CHANGE_IND 0x0032
#define UIM_REFRESH_IND 0x0033
#define UIM_SLOT_STATUS_IND 0x0048

#define NAS_GET_SIGNAL_STRENGTH 0x0020
#define NAS_INITIATE_REGISTER 0x0022
#define NAS_SCAN 0x0021
#define NAS_REGISTER_INDICATIONS 0x0003
#define NAS_GET_SERVING_SYSTEM 0x0024
#define NAS_SET_SYSTEM_PREFS 0x0033
#define NAS_GET_SYSTEM_PREFS 0x0034
#define NAS_GET_SIGNAL_INFO 0x004F
#define NAS_SIGNAL_INFO_IND 0x0051

static void copy_tlv_str(char *dst, size_t cap, qmi_span_t tl, uint8_t type)
{
    qmi_span_t v;
    dst[0] = '\0';
    if (qmi_tlv_find(tl, type, &v) == 1) qmi_copy_str(dst, cap, v.p, v.n);
}

/* ------------------------------------------------------------------ */
/* DMS                                                                 */
/* ------------------------------------------------------------------ */
int ril_modem_get_ids(ril_qmi_t *r, ril_ids_t *out)
{
    uint8_t rsp[512];
    size_t n = 0;
    int rc;
    qmi_span_t sp;
    memset(out, 0, sizeof(*out));
    rc = ril_svc(r, QMI_SVC_DMS, DMS_GET_IDS, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    copy_tlv_str(out->esn, sizeof(out->esn), sp, 0x10);
    copy_tlv_str(out->imei, sizeof(out->imei), sp, 0x11);
    copy_tlv_str(out->meid, sizeof(out->meid), sp, 0x12);
    copy_tlv_str(out->imeisv, sizeof(out->imeisv), sp, 0x13);
    if (ril_svc(r, QMI_SVC_DMS, DMS_GET_REVISION, NULL, 0, 0, 1, rsp, sizeof(rsp), &n) == 0) {
        sp.p = rsp;
        sp.n = n;
        copy_tlv_str(out->revision, sizeof(out->revision), sp, 0x01);
    }
    return 0;
}

int ril_modem_get_opmode(ril_qmi_t *r, int *mode)
{
    uint8_t rsp[64];
    size_t n = 0;
    qmi_span_t sp, v;
    int rc = ril_svc(r, QMI_SVC_DMS, DMS_GET_OPMODE, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x01, &v) != 1 || v.n < 1) return -EPROTO;
    *mode = v.p[0];
    return 0;
}

int ril_modem_set_opmode(ril_qmi_t *r, int mode)
{
    uint8_t tl[8];
    qmi_wbuf_t w;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x01, (uint8_t)mode);
    return ril_svcw(r, QMI_SVC_DMS, DMS_SET_OPMODE, &w, 10000, 0, NULL, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* UIM                                                                 */
/* ------------------------------------------------------------------ */
static int parse_card_status(qmi_span_t v, ril_card_t *c)
{
    qmi_rd_t rd;
    unsigned idx_gw, slots, s, want_slot, want_app;
    int found = 0, have_first = 0;
    ril_card_t first;
    memset(c, 0, sizeof(*c));
    memset(&first, 0, sizeof(first));
    c->pin1_state = c->pin2_state = 0;
    qmi_rd_init(&rd, v.p, v.n);
    idx_gw = qmi_rd_u16(&rd);
    c->provisioned = idx_gw != 0xFFFF;
    qmi_rd_u16(&rd);
    qmi_rd_u16(&rd);
    qmi_rd_u16(&rd);
    slots = qmi_rd_u8(&rd);
    /* index format [LIBQMI/QCRIL]: high byte = slot, low byte = application index; 0xFFFF = none */
    want_slot = idx_gw == 0xFFFF ? 0 : (idx_gw >> 8);
    want_app = idx_gw == 0xFFFF ? 0xFF : (idx_gw & 0xFF);
    for (s = 0; s < slots && qmi_rd_ok(&rd); s++) {
        unsigned card = qmi_rd_u8(&rd), upin = qmi_rd_u8(&rd), apps, a;
        qmi_rd_u8(&rd);
        qmi_rd_u8(&rd);
        qmi_rd_u8(&rd);
        apps = qmi_rd_u8(&rd);
        if (!qmi_rd_ok(&rd)) break;
        if (s == want_slot) {
            c->card_state = (int)card;
            c->present = card == 1;
            c->upin_state = (int)upin;
        }
        for (a = 0; a < apps && qmi_rd_ok(&rd); a++) {
            ril_card_t app;
            unsigned aidl;
            const uint8_t *aid;
            memset(&app, 0, sizeof(app));
            app.app_type = qmi_rd_u8(&rd);
            app.app_state = qmi_rd_u8(&rd);
            app.perso_state = qmi_rd_u8(&rd);
            qmi_rd_u8(&rd);
            qmi_rd_u8(&rd);
            qmi_rd_u8(&rd);
            aidl = qmi_rd_u8(&rd);
            aid = qmi_rd_bytes(&rd, aidl);
            app.aid_len = (int)(aidl > sizeof(app.aid) ? sizeof(app.aid) : aidl);
            if (aid && app.aid_len) memcpy(app.aid, aid, (size_t)app.aid_len);
            app.upin_replaces_pin1 = qmi_rd_u8(&rd);
            app.pin1_state = qmi_rd_u8(&rd);
            app.pin1_retries = qmi_rd_u8(&rd);
            app.puk1_retries = qmi_rd_u8(&rd);
            app.pin2_state = qmi_rd_u8(&rd);
            app.pin2_retries = qmi_rd_u8(&rd);
            app.puk2_retries = qmi_rd_u8(&rd);
            if (!qmi_rd_ok(&rd)) break;
            if (s == want_slot && !have_first && (app.app_type == 1 || app.app_type == 2)) {
                first = app;
                first.has_app = 1;
                have_first = 1;
            }
            if (s == want_slot && !found && (a == want_app || (want_app == 0xFF && (app.app_type == 1 || app.app_type == 2)))) {
                c->has_app = 1;
                c->app_type = app.app_type;
                c->app_state = app.app_state;
                c->perso_state = app.perso_state;
                c->upin_replaces_pin1 = app.upin_replaces_pin1;
                c->pin1_state = app.pin1_state;
                c->pin1_retries = app.pin1_retries;
                c->puk1_retries = app.puk1_retries;
                c->pin2_state = app.pin2_state;
                c->pin2_retries = app.pin2_retries;
                c->puk2_retries = app.puk2_retries;
                c->aid_len = app.aid_len;
                memcpy(c->aid, app.aid, sizeof(c->aid));
                c->slot = (int)s;
                found = 1;
            }
        }
    }
    if (!qmi_rd_ok(&rd)) return -EPROTO;
    if (!found && have_first) { /* the selected-application index pointed nowhere: use the first SIM/USIM */
        first.card_state = c->card_state;
        first.present = c->present;
        first.upin_state = c->upin_state;
        first.provisioned = c->provisioned;
        first.slot = (int)want_slot;
        *c = first;
    }
    return 0;
}

int ril_sim_status(ril_qmi_t *r, ril_card_t *out)
{
    uint8_t rsp[1024];
    size_t n = 0;
    qmi_span_t sp, v;
    int rc = ril_svc(r, QMI_SVC_UIM, UIM_GET_CARD_STATUS, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x10, &v) != 1) return -EPROTO;
    return parse_card_status(v, out);
}

int ril_sim_provision(ril_qmi_t *r)
{
    ril_card_t c;
    uint8_t tl[40], sess[2] = {0, 1}, app[18];
    qmi_wbuf_t w;
    int rc = ril_sim_status(r, &c);
    if (rc) return rc;
    if (!c.present || !c.has_app || c.aid_len <= 0 || (c.app_type != 1 && c.app_type != 2)) return -ENODEV;
    if (c.provisioned) return 0;
    /* [IDL qril_uim.qmi] TLV 0x01 {session_type 0 = primary GW, activate 1}; TLV 0x10 {slot (1-based), aid_len, aid} */
    app[0] = (uint8_t)(c.slot + 1);
    app[1] = (uint8_t)c.aid_len;
    memcpy(app + 2, c.aid, (size_t)c.aid_len);
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put(&w, 0x01, sess, 2);
    qmi_tlv_put(&w, 0x10, app, 2 + (size_t)c.aid_len);
    return ril_svcw(r, QMI_SVC_UIM, UIM_CHANGE_PROVISIONING_SESSION, &w, 10000, 0, NULL, 0, NULL);
}

int ril_sim_imsi(ril_qmi_t *r, char *out, size_t cap)
{
    uint8_t rsp[128];
    size_t n = 0;
    qmi_span_t sp;
    int rc = ril_svc(r, QMI_SVC_DMS, DMS_UIM_GET_IMSI, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    out[0] = '\0';
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    copy_tlv_str(out, cap, sp, 0x01);
    return out[0] ? 0 : -ENODATA;
}

int ril_sim_iccid(ril_qmi_t *r, char *out, size_t cap)
{
    uint8_t rsp[128];
    size_t n = 0;
    qmi_span_t sp;
    int rc = ril_svc(r, QMI_SVC_DMS, DMS_UIM_GET_ICCID, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    out[0] = '\0';
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    copy_tlv_str(out, cap, sp, 0x01);
    return out[0] ? 0 : -ENODATA;
}

static void put_session(qmi_wbuf_t *w, int session_type)
{
    uint8_t s[2] = {(uint8_t)session_type, 0};
    qmi_tlv_put(w, 0x01, s, 2);
}

static void retries_from(const uint8_t *rsp, size_t n, int *retries_left)
{
    qmi_span_t sp = {rsp, n}, v;
    if (!retries_left) return;
    *retries_left = -1;
    if (qmi_tlv_find(sp, 0x10, &v) == 1 && v.n >= 1) *retries_left = v.p[0];
}

static int pin_len_ok(const char *p) { return p && strlen(p) >= 4 && strlen(p) <= 8; }

int ril_sim_verify_pin(ril_qmi_t *r, int pin_id, const char *pin, int *retries_left)
{
    uint8_t tl[64], rsp[64], body[16];
    qmi_wbuf_t w;
    size_t n = 0, pl;
    int rc;
    if (!pin_len_ok(pin)) return -EINVAL;
    pl = strlen(pin);
    qmi_wbuf_init(&w, tl, sizeof(tl));
    put_session(&w, 0);
    body[0] = (uint8_t)pin_id;
    body[1] = (uint8_t)pl;
    memcpy(body + 2, pin, pl);
    qmi_tlv_put(&w, 0x02, body, 2 + pl);
    rc = ril_svcw(r, QMI_SVC_UIM, UIM_VERIFY_PIN, &w, 10000, 0, rsp, sizeof(rsp), &n);
    retries_from(rsp, n, retries_left);
    return rc;
}

int ril_sim_unblock_pin(ril_qmi_t *r, int pin_id, const char *puk, const char *new_pin, int *retries_left)
{
    uint8_t tl[96], rsp[64], body[32];
    qmi_wbuf_t w;
    size_t n = 0, kl, pl;
    int rc;
    if (!puk || strlen(puk) != 8 || !pin_len_ok(new_pin)) return -EINVAL;
    kl = strlen(puk);
    pl = strlen(new_pin);
    qmi_wbuf_init(&w, tl, sizeof(tl));
    put_session(&w, 0);
    body[0] = (uint8_t)pin_id;
    body[1] = (uint8_t)kl;
    memcpy(body + 2, puk, kl);
    body[2 + kl] = (uint8_t)pl;
    memcpy(body + 3 + kl, new_pin, pl);
    qmi_tlv_put(&w, 0x02, body, 3 + kl + pl);
    rc = ril_svcw(r, QMI_SVC_UIM, UIM_UNBLOCK_PIN, &w, 10000, 0, rsp, sizeof(rsp), &n);
    retries_from(rsp, n, retries_left);
    return rc;
}

int ril_sim_change_pin(ril_qmi_t *r, int pin_id, const char *old_pin, const char *new_pin, int *retries_left)
{
    uint8_t tl[96], rsp[64], body[32];
    qmi_wbuf_t w;
    size_t n = 0, ol, nl;
    int rc;
    if (!pin_len_ok(old_pin) || !pin_len_ok(new_pin)) return -EINVAL;
    ol = strlen(old_pin);
    nl = strlen(new_pin);
    qmi_wbuf_init(&w, tl, sizeof(tl));
    put_session(&w, 0);
    body[0] = (uint8_t)pin_id;
    body[1] = (uint8_t)ol;
    memcpy(body + 2, old_pin, ol);
    body[2 + ol] = (uint8_t)nl;
    memcpy(body + 3 + ol, new_pin, nl);
    qmi_tlv_put(&w, 0x02, body, 3 + ol + nl);
    rc = ril_svcw(r, QMI_SVC_UIM, UIM_CHANGE_PIN, &w, 10000, 0, rsp, sizeof(rsp), &n);
    retries_from(rsp, n, retries_left);
    return rc;
}

int ril_sim_set_pin_protection(ril_qmi_t *r, int pin_id, int enable, const char *pin, int *retries_left)
{
    uint8_t tl[64], rsp[64], body[16];
    qmi_wbuf_t w;
    size_t n = 0, pl;
    int rc;
    if (!pin_len_ok(pin)) return -EINVAL;
    pl = strlen(pin);
    qmi_wbuf_init(&w, tl, sizeof(tl));
    put_session(&w, 0);
    body[0] = (uint8_t)pin_id;
    body[1] = enable ? 1 : 0;
    body[2] = (uint8_t)pl;
    memcpy(body + 3, pin, pl);
    qmi_tlv_put(&w, 0x02, body, 3 + pl);
    rc = ril_svcw(r, QMI_SVC_UIM, UIM_SET_PIN_PROTECTION, &w, 10000, 0, rsp, sizeof(rsp), &n);
    retries_from(rsp, n, retries_left);
    return rc;
}

/* Android path "3F00 7FFF" (big endian ids) -> QMI "00 3F FF 7F" (little endian ids) */
static int build_file_tlv(qmi_wbuf_t *w, int file_id, const uint8_t *path, size_t path_len)
{
    uint8_t body[64];
    size_t i, o = 0;
    if (path_len % 2 || path_len > 16) return -EINVAL;
    body[o++] = (uint8_t)file_id;
    body[o++] = (uint8_t)(file_id >> 8);
    body[o++] = (uint8_t)path_len;
    for (i = 0; i < path_len; i += 2) {
        body[o++] = path[i + 1];
        body[o++] = path[i];
    }
    qmi_tlv_put(w, 0x02, body, o);
    return 0;
}

/* session 0 = primary GSM/WCDMA provisioning; files that live directly under the MF use "card slot 1" (6) */
static int session_for_path(const uint8_t *path, size_t path_len)
{
    return path_len == 2 && path[0] == 0x3F && path[1] == 0x00 ? 6 : 0;
}

static int icc_common(ril_qmi_t *r, uint16_t msg, int file_id, const uint8_t *path, size_t path_len, int a, int b,
                      int has_read_info, ril_icc_result_t *out)
{
    uint8_t tl[128], rsp[1024];
    qmi_wbuf_t w;
    qmi_span_t sp, v;
    size_t n = 0;
    int rc;
    memset(out, 0, sizeof(*out));
    qmi_wbuf_init(&w, tl, sizeof(tl));
    put_session(&w, session_for_path(path, path_len));
    rc = build_file_tlv(&w, file_id, path, path_len);
    if (rc) return rc;
    if (has_read_info) {
        uint8_t ri[4] = {(uint8_t)a, (uint8_t)(a >> 8), (uint8_t)b, (uint8_t)(b >> 8)};
        qmi_tlv_put(&w, 0x03, ri, 4);
    }
    rc = ril_svcw(r, QMI_SVC_UIM, msg, &w, 8000, 0, rsp, sizeof(rsp), &n);
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x10, &v) == 1 && v.n >= 2) {
        out->sw1 = v.p[0];
        out->sw2 = v.p[1];
    }
    if (rc) return rc;
    if (msg == UIM_GET_FILE_ATTRS) {
        if (qmi_tlv_find(sp, 0x11, &v) == 1 && v.n >= 26) {
            size_t rawlen = (size_t)v.p[24] | ((size_t)v.p[25] << 8);
            if (rawlen > v.n - 26) rawlen = v.n - 26;
            if (rawlen > sizeof(out->data)) rawlen = sizeof(out->data);
            memcpy(out->data, v.p + 26, rawlen);
            out->len = rawlen;
        }
    } else if (qmi_tlv_find(sp, 0x11, &v) == 1 && v.n >= 2) {
        size_t len = (size_t)v.p[0] | ((size_t)v.p[1] << 8);
        if (len > v.n - 2) len = v.n - 2;
        if (len > sizeof(out->data)) len = sizeof(out->data);
        memcpy(out->data, v.p + 2, len);
        out->len = len;
    }
    return 0;
}

int ril_sim_read_transparent(ril_qmi_t *r, int file_id, const uint8_t *path, size_t path_len, int offset, int length,
                             ril_icc_result_t *out)
{
    return icc_common(r, UIM_READ_TRANSPARENT, file_id, path, path_len, offset, length, 1, out);
}

int ril_sim_read_record(ril_qmi_t *r, int file_id, const uint8_t *path, size_t path_len, int record, int length,
                        ril_icc_result_t *out)
{
    return icc_common(r, UIM_READ_RECORD, file_id, path, path_len, record, length, 1, out);
}

int ril_sim_get_file_attrs(ril_qmi_t *r, int file_id, const uint8_t *path, size_t path_len, ril_icc_result_t *out)
{
    return icc_common(r, UIM_GET_FILE_ATTRS, file_id, path, path_len, 0, 0, 0, out);
}

void ril_uim_register(struct ril_qmi *r)
{
    uint8_t tl[8];
    qmi_wbuf_t w;
    int rc;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u32(&w, 0x01, 0x07);
    rc = ril_svcw(r, QMI_SVC_UIM, UIM_EVENT_REG, &w, 0, 0, NULL, 0, NULL);
    if (rc) {
        qmi_wbuf_init(&w, tl, sizeof(tl));
        qmi_tlv_put_u32(&w, 0x01, 0x01);
        rc = ril_svcw(r, QMI_SVC_UIM, UIM_EVENT_REG, &w, 0, 0, NULL, 0, NULL);
    }
    if (rc) qmi_logf(QMI_LOG_WARN, "ril: UIM event registration failed (%d), SIM changes will not be pushed", rc);
}

void ril_uim_on_indication(struct ril_qmi *r, const qmi_msg_t *m)
{
    if (m->msg_id == UIM_STATUS_CHANGE_IND || m->msg_id == UIM_REFRESH_IND || m->msg_id == UIM_SLOT_STATUS_IND)
        ril_ev_push(r, EV_SIM, 0, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* NAS                                                                 */
/* ------------------------------------------------------------------ */
/* The PLMN description of NAS 0x12 is not plain ASCII on the real modem: "D2 E7 94 5A 64 16 87 CF 26" is GSM 7-bit packed "ROSTELECOM".
 * Rule [from ONE sample, UNVERIFIED elsewhere]: any byte >= 0x80 => GSM 7-bit packed (only the part of the alphabet that equals
 * ASCII is decoded; anything else becomes '?'); otherwise printable ASCII is copied; anything else yields an empty name
 * (the HAL then falls back to the numeric PLMN). */
static void operator_name_decode(char *out, size_t cap, const uint8_t *s, size_t n)
{
    size_t i, o = 0;
    int packed = 0;
    if (!cap) return;
    out[0] = '\0';
    for (i = 0; i < n; i++) if (s[i] >= 0x80) packed = 1;
    if (!packed) {
        for (i = 0; i < n; i++) if (s[i] < 0x20 || s[i] > 0x7E) return;
        qmi_copy_str(out, cap, s, n);
        return;
    }
    {
        uint32_t bits = 0;
        unsigned have = 0;
        for (i = 0; i < n && o + 1 < cap; i++) {
            bits |= (uint32_t)s[i] << have;
            have += 8;
            while (have >= 7 && o + 1 < cap) {
                unsigned c = bits & 0x7F;
                bits >>= 7;
                have -= 7;
                out[o++] = (c >= 0x20 && c <= 0x7E) ? (char)c : (c == 0 ? '@' : '?');
            }
        }
        while (o > 0 && out[o - 1] == '@') o--; /* trailing zero septets are padding */
        out[o] = '\0';
    }
}

int ril_net_serving(ril_qmi_t *r, ril_serving_t *out)
{
    uint8_t rsp[512];
    size_t n = 0;
    qmi_span_t sp, v;
    int rc;
    memset(out, 0, sizeof(*out));
    out->roaming = -1;
    out->reg_state = 4;
    rc = ril_svc(r, QMI_SVC_NAS, NAS_GET_SERVING_SYSTEM, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    /* Seen on the real modem (MPSS.DPM.2.0.2, no service): result=FAILURE/INFO_UNAVAILABLE (0x4A), yet TLV 0x01
     * is present and says "not registered". That is a valid answer, not a failed request. */
    if (rc == -(QMI_E_BASE + QMI_ERR_INFO_UNAVAILABLE)) rc = 0;
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x01, &v) == 1 && v.n >= 5) {
        out->reg_state = v.p[0];
        out->cs_attach = v.p[1];
        out->ps_attach = v.p[2];
        if (v.p[4] > 0 && v.n >= 6) {
            switch (v.p[5]) {
            case 4: out->rat = RIL_RAT_GSM; break;
            case 5: out->rat = RIL_RAT_UMTS; break;
            case 8: out->rat = RIL_RAT_LTE; break;
            default: out->rat = RIL_RAT_NONE;
            }
        }
    }
    if (qmi_tlv_find(sp, 0x12, &v) == 1 && v.n >= 5) {
        qmi_rd_t rd;
        unsigned l;
        const uint8_t *s;
        qmi_rd_init(&rd, v.p, v.n);
        out->mcc = qmi_rd_u16(&rd);
        out->mnc = qmi_rd_u16(&rd);
        l = qmi_rd_u8(&rd);
        s = qmi_rd_bytes(&rd, l);
        if (qmi_rd_ok(&rd)) {
            out->plmn_valid = out->mcc != 0;
            if (s) operator_name_decode(out->operator_name, sizeof(out->operator_name), s, l);
        }
    }
    /* Layout seen on the real modem while registered on LTE: TLV 0x1C (2 bytes) = FE FF, TLV 0x1D (4 bytes) = 0C 3C 51 09
     * (0x09513C0C, fits a 28-bit LTE cell id). The libqril IDL says 0x1D = u16 LAC and 0x1E = u16 CID, which does not match.
     * Rule used here [from ONE sample, UNVERIFIED elsewhere]: 0x1D with 4 bytes is the cell id, 0x1C is the LAC; the IDL
     * forms are kept as fallbacks. LAC 0xFFFE/0xFFFF means "none" (LTE has no LAC). */
    if (qmi_tlv_find(sp, 0x1C, &v) == 1 && v.n == 2) {
        unsigned lac = (unsigned)(v.p[0] | (v.p[1] << 8));
        if (lac != 0xFFFE && lac != 0xFFFF) { out->lac_valid = 1; out->lac = (int)lac; }
    }
    if (qmi_tlv_find(sp, 0x1D, &v) == 1) {
        if (v.n == 4) {
            out->cid_valid = 1;
            out->cid = (uint32_t)(v.p[0] | (v.p[1] << 8) | (v.p[2] << 16) | ((uint32_t)v.p[3] << 24));
        } else if (v.n == 2 && !out->lac_valid) {
            out->lac_valid = 1;
            out->lac = v.p[0] | (v.p[1] << 8);
        }
    }
    if (!out->cid_valid && qmi_tlv_find(sp, 0x1E, &v) == 1 && (v.n == 4 || v.n == 2)) {
        out->cid_valid = 1;
        out->cid = v.n == 4 ? (uint32_t)(v.p[0] | (v.p[1] << 8) | (v.p[2] << 16) | ((uint32_t)v.p[3] << 24))
                            : (uint32_t)(v.p[0] | (v.p[1] << 8));
    }
    return 0;
}

int ril_net_signal(ril_qmi_t *r, ril_signal_t *out)
{
    uint8_t rsp[512];
    size_t n = 0;
    qmi_span_t sp, v;
    int rc;
    memset(out, 0, sizeof(*out));
    rc = ril_svc(r, QMI_SVC_NAS, NAS_GET_SIGNAL_INFO, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    if (rc == 0) {
        sp.p = rsp;
        sp.n = n;
        if (qmi_tlv_find(sp, 0x12, &v) == 1 && v.n >= 1) {
            out->gsm_valid = 1;
            out->gsm_rssi = (int8_t)v.p[0];
        }
        if (qmi_tlv_find(sp, 0x13, &v) == 1 && v.n >= 3) {
            out->wcdma_valid = 1;
            out->wcdma_rssi = (int8_t)v.p[0];
            out->wcdma_ecio = (int16_t)(v.p[1] | (v.p[2] << 8));
        }
        if (qmi_tlv_find(sp, 0x14, &v) == 1 && v.n >= 6) {
            out->lte_valid = 1;
            out->lte_rssi = (int8_t)v.p[0];
            out->lte_rsrq = (int8_t)v.p[1];
            out->lte_rsrp = (int16_t)(v.p[2] | (v.p[3] << 8));
            out->lte_snr = (int16_t)(v.p[4] | (v.p[5] << 8));
        }
        return 0;
    }
    /* Real modem (MPSS.DPM.2.0.2, no service): 0x4F answers FAILURE/INFO_UNAVAILABLE. That is "no signal info", not a failed request. */
    if (rc == -(QMI_E_BASE + QMI_ERR_INFO_UNAVAILABLE)) return 0;
    if (QMI_E_IS_QMI(rc) && QMI_E_CODE(rc) != QMI_ERR_INVALID_QMI_CMD && QMI_E_CODE(rc) != QMI_ERR_OP_DEVICE_UNSUPPORTED)
        return rc; /* e.g. no service: do not mask it with the fallback */
    {
        uint8_t tl[8];
        qmi_wbuf_t w;
        qmi_wbuf_init(&w, tl, sizeof(tl));
        qmi_tlv_put_u16(&w, 0x10, 0x00FF);
        rc = ril_svcw(r, QMI_SVC_NAS, NAS_GET_SIGNAL_STRENGTH, &w, 0, 1, rsp, sizeof(rsp), &n);
        if (rc) return rc;
        sp.p = rsp;
        sp.n = n;
        if (qmi_tlv_find(sp, 0x01, &v) == 1 && v.n >= 2) {
            out->generic_valid = 1;
            out->generic_dbm = (int8_t)v.p[0];
            out->generic_rat = (int8_t)v.p[1];
        }
    }
    return 0;
}

int ril_net_register_auto(ril_qmi_t *r)
{
    uint8_t tl[8];
    qmi_wbuf_t w;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x01, 1);
    return ril_svcw(r, QMI_SVC_NAS, NAS_INITIATE_REGISTER, &w, 15000, 0, NULL, 0, NULL);
}

int ril_net_register_manual(ril_qmi_t *r, int mcc, int mnc, int rat)
{
    uint8_t tl[32], b[5];
    qmi_wbuf_t w;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x01, 2);
    b[0] = (uint8_t)mcc;
    b[1] = (uint8_t)(mcc >> 8);
    b[2] = (uint8_t)mnc;
    b[3] = (uint8_t)(mnc >> 8);
    b[4] = (uint8_t)rat;
    qmi_tlv_put(&w, 0x10, b, 5);
    return ril_svcw(r, QMI_SVC_NAS, NAS_INITIATE_REGISTER, &w, 60000, 0, NULL, 0, NULL);
}

int ril_net_set_mode_pref(ril_qmi_t *r, unsigned mask)
{
    uint8_t tl[16];
    qmi_wbuf_t w;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u16(&w, 0x11, (uint16_t)mask); /* [IDL] set_system_prefs.mode_preference */
    return ril_svcw(r, QMI_SVC_NAS, NAS_SET_SYSTEM_PREFS, &w, 10000, 0, NULL, 0, NULL);
}

int ril_net_get_mode_pref(ril_qmi_t *r, unsigned *mask)
{
    uint8_t rsp[128];
    size_t n = 0;
    qmi_span_t sp, v;
    int rc = ril_svc(r, QMI_SVC_NAS, NAS_GET_SYSTEM_PREFS, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x11, &v) != 1 || v.n < 2) return -ENODATA; /* [UNVERIFIED] libqmi: mode preference TLV 0x11 */
    *mask = (unsigned)(v.p[0] | (v.p[1] << 8));
    return 0;
}

int ril_net_get_selection_mode(ril_qmi_t *r, int *manual)
{
    uint8_t rsp[128];
    size_t n = 0;
    qmi_span_t sp, v;
    int rc = ril_svc(r, QMI_SVC_NAS, NAS_GET_SYSTEM_PREFS, NULL, 0, 0, 1, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    *manual = 0;
    if (qmi_tlv_find(sp, 0x16, &v) == 1 && v.n >= 1) *manual = v.p[0] == 1;
    return 0;
}

int ril_net_scan(ril_qmi_t *r, ril_scan_entry_t *out, size_t max, size_t *count)
{
    uint8_t rsp[QMI_MAX_FRAME - 64];
    size_t n = 0, i;
    qmi_span_t sp, v;
    int rc;
    *count = 0;
    rc = ril_svc(r, QMI_SVC_NAS, NAS_SCAN, NULL, 0, 240000, 0, rsp, sizeof(rsp), &n);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = n;
    if (qmi_tlv_find(sp, 0x10, &v) == 1 && v.n >= 2) {
        qmi_rd_t rd;
        unsigned cnt;
        qmi_rd_init(&rd, v.p, v.n);
        cnt = qmi_rd_u16(&rd);
        for (i = 0; i < cnt && *count < max && qmi_rd_ok(&rd); i++) {
            ril_scan_entry_t *e = &out[*count];
            unsigned dl;
            const uint8_t *d;
            memset(e, 0, sizeof(*e));
            e->mcc = qmi_rd_u16(&rd);
            e->mnc = qmi_rd_u16(&rd);
            e->status = qmi_rd_u8(&rd);
            dl = qmi_rd_u8(&rd);
            d = qmi_rd_bytes(&rd, dl);
            if (!qmi_rd_ok(&rd)) break;
            if (d) qmi_copy_str(e->name, sizeof(e->name), d, dl);
            (*count)++;
        }
    }
    if (qmi_tlv_find(sp, 0x11, &v) == 1 && v.n >= 2) {
        qmi_rd_t rd;
        unsigned cnt;
        qmi_rd_init(&rd, v.p, v.n);
        cnt = qmi_rd_u16(&rd);
        for (i = 0; i < cnt && qmi_rd_ok(&rd); i++) {
            unsigned mcc = qmi_rd_u16(&rd), mnc = qmi_rd_u16(&rd), rat = qmi_rd_u8(&rd);
            size_t k;
            if (!qmi_rd_ok(&rd)) break;
            for (k = 0; k < *count; k++)
                if ((unsigned)out[k].mcc == mcc && (unsigned)out[k].mnc == mnc && out[k].rat == 0) {
                    out[k].rat = (int)rat;
                    break;
                }
        }
    }
    return 0;
}

void ril_nas_register(struct ril_qmi *r)
{
    uint8_t tl[32];
    qmi_wbuf_t w;
    int rc;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x10, 1); /* system selection preference */
    qmi_tlv_put_u8(&w, 0x13, 1); /* serving system events */
    qmi_tlv_put_u8(&w, 0x18, 1); /* system info */
    qmi_tlv_put_u8(&w, 0x19, 1); /* signal info */
    qmi_tlv_put_u8(&w, 0x1E, 1); /* current PLMN name */
    rc = ril_svcw(r, QMI_SVC_NAS, NAS_REGISTER_INDICATIONS, &w, 0, 0, NULL, 0, NULL);
    if (rc) qmi_logf(QMI_LOG_WARN, "ril: NAS indication registration failed (%d), relying on polling", rc);
}

void ril_nas_on_indication(struct ril_qmi *r, const qmi_msg_t *m)
{
    switch (m->msg_id) {
    case 0x0002: /* event report (signal strength) */
    case NAS_SIGNAL_INFO_IND:
        ril_ev_push(r, EV_SIG, 0, NULL, 0);
        break;
    case 0x0024: /* serving system report [IDL] */
    case 0x003A: /* operator name [IDL] */
    case 0x004E: /* system info */
        ril_ev_push(r, EV_NET, 0, NULL, 0);
        break;
    default: break;
    }
}
