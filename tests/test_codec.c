/* SPDX-License-Identifier: Apache-2.0
 * Host tests for the QMUX/QMI codec. Golden vectors 1 and 2 are REAL frames
 * captured from a Qualcomm modem (UIM Get File Attributes), copied from
 * TB-X505X-Labs/android_external_qrild notes/dev-notes.md. */
#include <stdio.h>
#include <string.h>

#include "qmi/qmi_codec.h"

static int g_fail, g_run;
#define CHECK(c)                                                         \
    do {                                                                 \
        g_run++;                                                         \
        if (!(c)) {                                                      \
            g_fail++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
        }                                                                \
    } while (0)

static const uint8_t REQ[] = {0x01, 0x1B, 0x00, 0x00, 0x0B, 0x01, 0x00, 0x01, 0x00, 0x24,
                              0x00, 0x0F, 0x00, 0x02, 0x07, 0x00, 0x62, 0x6F, 0x04, 0x00,
                              0x3F, 0xFF, 0x7F, 0x01, 0x02, 0x00, 0x00, 0x00};
static const uint8_t RSP_HEAD[] = {0x01, 0x5A, 0x00, 0x00, 0x0B, 0x01, 0x02, 0x01, 0x00, 0x24, 0x00, 0x4E, 0x00};

static void test_real_request(void)
{
    qmi_msg_t m;
    uint8_t out[64];
    size_t n;
    CHECK(sizeof(REQ) == 28);
    CHECK(qmi_msg_decode(REQ, sizeof(REQ), &m) == 0);
    CHECK(m.service == QMI_SVC_UIM && m.client_id == 1);
    CHECK(m.type == QMI_MSG_REQUEST && m.txid == 1 && m.msg_id == 0x0024);
    CHECK(m.tlvs.n == 15);
    CHECK(qmi_tlv_validate(m.tlvs) == 0);
    /* re-encode must reproduce the captured bytes exactly */
    n = qmi_msg_encode(out, sizeof(out), m.service, m.client_id, m.type, m.txid, m.msg_id, m.tlvs.p, m.tlvs.n);
    CHECK(n == sizeof(REQ) && memcmp(out, REQ, n) == 0);
}

static void test_real_response_header(void)
{
    /* Only the 13 header bytes of the captured response are used; build a frame
     * with the same header and a result TLV to check type/flags decoding. */
    uint8_t tl[] = {0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t f[64];
    qmi_msg_t m;
    qmi_resp_t r;
    size_t n = qmi_msg_encode(f, sizeof(f), QMI_SVC_UIM, 1, QMI_MSG_RESPONSE, 1, 0x0024, tl, sizeof(tl));
    CHECK(n == 6 + 7 + sizeof(tl));
    /* RSP_HEAD[3] (QMUX flags) is 0x00 in the capture because that capture went
     * through libqrtr, which synthesises the QMUX header. On a raw QMUX device
     * the modem sets 0x80; the decoder therefore ignores this byte. */
    CHECK(f[6] == RSP_HEAD[6]);                                    /* QMI flags 0x02 == response */
    CHECK(f[7] == RSP_HEAD[7] && f[8] == RSP_HEAD[8]);             /* txid 0x0001 */
    CHECK(f[9] == RSP_HEAD[9] && f[10] == RSP_HEAD[10]);           /* msg 0x0024 */
    CHECK(qmi_msg_decode(f, n, &m) == 0 && m.type == QMI_MSG_RESPONSE);
    CHECK(qmi_resp_parse(m.tlvs, &r) == 0 && qmi_resp_ok(&r));
}

static void test_ctl(void)
{
    /* CTL Allocate CID for NAS (service 3): well known libqmi frame. */
    static const uint8_t exp[] = {0x01, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x22, 0x00, 0x04, 0x00, 0x01, 0x01, 0x00, 0x03};
    uint8_t tl[8], out[32];
    qmi_wbuf_t w;
    qmi_msg_t m;
    size_t n;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x01, QMI_SVC_NAS);
    CHECK(qmi_wbuf_ok(&w));
    n = qmi_msg_encode(out, sizeof(out), QMI_SVC_CTL, 0, QMI_MSG_REQUEST, 1, 0x0022, tl, w.len);
    CHECK(n == sizeof(exp) && memcmp(out, exp, n) == 0);
    CHECK(qmi_msg_decode(out, n, &m) == 0 && m.service == QMI_SVC_CTL && m.txid == 1 && m.msg_id == 0x22);
    /* CTL response flag is 0x01, indication 0x02 */
    n = qmi_msg_encode(out, sizeof(out), QMI_SVC_CTL, 0, QMI_MSG_RESPONSE, 7, 0x0022, NULL, 0);
    CHECK(out[6] == 0x01 && out[3] == 0x80);
    CHECK(qmi_msg_decode(out, n, &m) == 0 && m.type == QMI_MSG_RESPONSE && m.txid == 7);
}

static void test_tlv(void)
{
    uint8_t b[64];
    qmi_wbuf_t w;
    qmi_span_t all, v;
    size_t mk, off = 0;
    uint8_t ty;
    qmi_wbuf_init(&w, b, sizeof(b));
    qmi_tlv_put_u16(&w, 0x10, 0x1234);
    qmi_tlv_begin(&w, 0x11, &mk);
    qmi_wbuf_u8(&w, 9);
    qmi_wbuf_u32(&w, 0xAABBCCDD);
    qmi_tlv_end(&w, mk);
    qmi_tlv_put_str(&w, 0x12, "abc");
    CHECK(qmi_wbuf_ok(&w));
    all.p = b;
    all.n = w.len;
    CHECK(qmi_tlv_validate(all) == 0);
    CHECK(qmi_tlv_find(all, 0x11, &v) == 1 && v.n == 5 && v.p[0] == 9 && v.p[1] == 0xDD);
    CHECK(qmi_tlv_find(all, 0x77, &v) == 0);
    CHECK(qmi_tlv_next(all, &off, &ty, &v) == 1 && ty == 0x10);
    /* truncated TLV region */
    all.n -= 1;
    CHECK(qmi_tlv_validate(all) == -1);
    CHECK(qmi_tlv_find(all, 0x77, &v) == -1);
    /* overflow is sticky */
    qmi_wbuf_init(&w, b, 4);
    CHECK(qmi_tlv_put_u32(&w, 1, 5) == -1 && !qmi_wbuf_ok(&w));
}

static void test_reader(void)
{
    const uint8_t d[] = {1, 2, 3};
    qmi_rd_t r;
    qmi_rd_init(&r, d, sizeof(d));
    CHECK(qmi_rd_u16(&r) == 0x0201 && qmi_rd_ok(&r));
    CHECK(qmi_rd_u16(&r) == 0 && !qmi_rd_ok(&r));
    CHECK(qmi_rd_u8(&r) == 0); /* sticky error */
    {
        char s[4];
        CHECK(qmi_copy_str(s, sizeof(s), (const uint8_t *)"abcdef", 6) == 3 && !strcmp(s, "abc"));
    }
}

static void test_decode_errors(void)
{
    qmi_msg_t m;
    uint8_t bad[sizeof(REQ)];
    CHECK(qmi_msg_decode(REQ, 5, &m) == QMI_DEC_SHORT);
    memcpy(bad, REQ, sizeof(bad));
    bad[0] = 0x02;
    CHECK(qmi_msg_decode(bad, sizeof(bad), &m) == QMI_DEC_MARKER);
    memcpy(bad, REQ, sizeof(bad));
    bad[1] = 0x20;
    CHECK(qmi_msg_decode(bad, sizeof(bad), &m) == QMI_DEC_LENGTH);
    memcpy(bad, REQ, sizeof(bad));
    bad[11] = 0x40; /* tlv_len larger than frame */
    CHECK(qmi_msg_decode(bad, sizeof(bad), &m) == QMI_DEC_TLVLEN);
}

static qmi_deframer_t g_df; /* ~8 KiB: keep off the stack */

static void test_deframer(void)
{
    uint8_t a[64], b[64], stream[256];
    size_t na, nb, i, got = 0, pos = 0;
    qmi_msg_t m;
    uint8_t tl[] = {0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};

    na = qmi_msg_encode(a, sizeof(a), QMI_SVC_DMS, 1, QMI_MSG_RESPONSE, 0x0102, 0x0025, tl, sizeof(tl));
    nb = qmi_msg_encode(b, sizeof(b), QMI_SVC_NAS, 2, QMI_MSG_INDICATION, 0, 0x0051, NULL, 0);

    /* 1) byte-by-byte delivery of two frames */
    memcpy(stream, a, na);
    memcpy(stream + na, b, nb);
    qmi_deframer_init(&g_df);
    for (i = 0; i < na + nb; i++) {
        CHECK(qmi_deframer_push(&g_df, stream + i, 1) == 1);
        while (qmi_deframer_pop(&g_df, &m)) {
            if (got == 0) CHECK(m.service == QMI_SVC_DMS && m.txid == 0x0102 && m.msg_id == 0x25 && m.type == QMI_MSG_RESPONSE);
            if (got == 1) CHECK(m.service == QMI_SVC_NAS && m.type == QMI_MSG_INDICATION && m.msg_id == 0x51);
            got++;
        }
    }
    CHECK(got == 2 && g_df.resyncs == 0 && g_df.dropped_bytes == 0);

    /* 2) both frames coalesced in a single read */
    qmi_deframer_init(&g_df);
    CHECK(qmi_deframer_push(&g_df, stream, na + nb) == na + nb);
    CHECK(qmi_deframer_pop(&g_df, &m) == 1 && m.service == QMI_SVC_DMS);
    CHECK(qmi_deframer_pop(&g_df, &m) == 1 && m.service == QMI_SVC_NAS);
    CHECK(qmi_deframer_pop(&g_df, &m) == 0);

    /* 3) garbage in front and between frames must be skipped and counted */
    qmi_deframer_init(&g_df);
    stream[pos++] = 0xAA;
    stream[pos++] = 0x55;
    memcpy(stream + pos, a, na);
    pos += na;
    stream[pos++] = 0x00;
    memcpy(stream + pos, b, nb);
    pos += nb;
    CHECK(qmi_deframer_push(&g_df, stream, pos) == pos);
    CHECK(qmi_deframer_pop(&g_df, &m) == 1 && m.service == QMI_SVC_DMS);
    CHECK(qmi_deframer_pop(&g_df, &m) == 1 && m.service == QMI_SVC_NAS);
    CHECK(g_df.dropped_bytes == 3 && g_df.resyncs >= 2);

    /* 4) a false marker with an impossible length must not wedge the stream */
    qmi_deframer_init(&g_df);
    {
        uint8_t junk[] = {0x01, 0xFF, 0xFF};
        CHECK(qmi_deframer_push(&g_df, junk, sizeof(junk)) == sizeof(junk));
        CHECK(qmi_deframer_push(&g_df, a, na) == na);
        CHECK(qmi_deframer_pop(&g_df, &m) == 1 && m.service == QMI_SVC_DMS);
    }

    /* 5) partial frame stays buffered until completed */
    qmi_deframer_init(&g_df);
    CHECK(qmi_deframer_push(&g_df, a, na - 1) == na - 1);
    CHECK(qmi_deframer_pop(&g_df, &m) == 0);
    CHECK(qmi_deframer_push(&g_df, a + na - 1, 1) == 1);
    CHECK(qmi_deframer_pop(&g_df, &m) == 1);
}

/* Frames captured from the REAL modem of a Galaxy J5 2016 over /dev/wwan0qmi0 (MPSS.DPM.2.0.2.c1.7, 2026-11-05). */
static void test_device_frames(void)
{
    static const uint8_t CTL_SYNC_IND[] = {0x01,0x0B,0x00,0x80,0x00,0x00,0x02,0x00,0x27,0x00,0x00,0x00};
    static const uint8_t NAS_NOSVC[] = {0x01,0x1B,0x00,0x80,0x03,0x01,0x02,0x01,0x00,0x24,0x00,0x0F,0x00,
                                        0x02,0x04,0x00,0x01,0x00,0x4A,0x00,0x01,0x05,0x00,0x00,0x00,0x00,0x00,0x00};
    static const uint8_t UIM_CARD[] = {0x01,0x37,0x00,0x80,0x0B,0x01,0x02,0x01,0x00,0x2F,0x00,0x2B,0x00,0x02,0x04,0x00,
                                       0x00,0x00,0x00,0x00,0x15,0x03,0x00,0x02,0x00,0x00,0x11,0x03,0x00,0x02,0x00,0x00,
                                       0x10,0x15,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x02,0x02,0x00,0x00,0x00,
                                       0x03,0x00,0x02,0x00,0x00,0x00,0x03,0x00};
    qmi_msg_t m;
    qmi_resp_t r;
    qmi_span_t v;
    CHECK(qmi_msg_decode(CTL_SYNC_IND, sizeof(CTL_SYNC_IND), &m) == 0);
    CHECK(m.service == QMI_SVC_CTL && m.type == QMI_MSG_INDICATION && m.msg_id == 0x0027 && m.tlvs.n == 0);
    CHECK(qmi_msg_decode(NAS_NOSVC, sizeof(NAS_NOSVC), &m) == 0 && m.type == QMI_MSG_RESPONSE && m.service == QMI_SVC_NAS);
    CHECK(qmi_resp_parse(m.tlvs, &r) == 0 && r.has_result && !qmi_resp_ok(&r) && r.error == QMI_ERR_INFO_UNAVAILABLE);
    CHECK(qmi_tlv_find(m.tlvs, 0x01, &v) == 1 && v.n == 5);
    CHECK(qmi_msg_decode(UIM_CARD, sizeof(UIM_CARD), &m) == 0 && m.service == QMI_SVC_UIM && m.msg_id == 0x002F);
    CHECK(qmi_resp_parse(m.tlvs, &r) == 0 && qmi_resp_ok(&r));
    CHECK(qmi_tlv_find(m.tlvs, 0x10, &v) == 1 && v.n == 0x15 && v.p[8] == 2 /* slots */ && v.p[9] == 2 /* card_state=error */);
}

int main(void)
{
    test_real_request();
    test_real_response_header();
    test_ctl();
    test_tlv();
    test_reader();
    test_decode_errors();
    test_deframer();
    test_device_frames();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
