/* SPDX-License-Identifier: Apache-2.0
 *
 * qmi_codec.h - QMUX/QMI framing and TLV helpers (pure, no I/O, no malloc).
 *
 * Wire format implemented here (QMI over a raw QMUX character device such as
 * /dev/wwan0qmi0 exported by the rpmsg_wwan_ctrl kernel driver):
 *
 *   QMUX:  u8  marker  = 0x01
 *          u16 length  = bytes that follow the marker (little endian)
 *          u8  flags   = 0x00 control point -> modem, 0x80 modem -> control point
 *          u8  service
 *          u8  client_id   (0 for CTL, 0xFF = broadcast for indications)
 *   QMI SDU, CTL service:     u8 flags, u8  txid, u16 msg_id, u16 tlv_len
 *   QMI SDU, other services:  u8 flags, u16 txid, u16 msg_id, u16 tlv_len
 *       CTL flags:      0x00 request, 0x01 response, 0x02 indication
 *       service flags:  0x00 request, 0x02 response, 0x04 indication
 *   TLV:   u8 type, u16 length, value[length]
 *
 * NOTE: this layout follows the public QMI conventions (as used by libqmi) and
 * is cross-checked on the host by tests/test_codec.c.  It must still be
 * confirmed against the real modem with tools/qmi-probe (see README_RU.md).
 */
#ifndef QMI_CODEC_H
#define QMI_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Service identifiers                                                 */
/* ------------------------------------------------------------------ */
enum qmi_service {
    QMI_SVC_CTL = 0x00,
    QMI_SVC_WDS = 0x01,
    QMI_SVC_DMS = 0x02,
    QMI_SVC_NAS = 0x03,
    QMI_SVC_QOS = 0x04,
    QMI_SVC_WMS = 0x05,
    QMI_SVC_PDS = 0x06,
    QMI_SVC_AUTH = 0x07,
    QMI_SVC_AT = 0x08,
    QMI_SVC_VOICE = 0x09,
    QMI_SVC_CAT2 = 0x0A,
    QMI_SVC_UIM = 0x0B,
    QMI_SVC_PBM = 0x0C,
    QMI_SVC_LOC = 0x10,
    QMI_SVC_SAR = 0x11,
    QMI_SVC_WDA = 0x1A,
    QMI_SVC_COEX = 0x22,
    QMI_SVC_PDC = 0x24,
    QMI_SVC_DSD = 0x2A
};

const char *qmi_service_name(uint8_t service); /* never NULL */

/* ------------------------------------------------------------------ */
/* Limits and constants                                                */
/* ------------------------------------------------------------------ */
#define QMI_QMUX_MARKER 0x01
#define QMI_MAX_FRAME 8192 /* hard upper bound for one QMUX frame */
#define QMI_CLIENT_BROADCAST 0xFF

#define QMI_RESULT_SUCCESS 0
#define QMI_RESULT_FAILURE 1
#define QMI_TLV_RESULT 0x02 /* standard "result code" TLV of every response */

/* A selection of QMI error codes (qmi_error field of the result TLV). */
enum qmi_error_code {
    QMI_ERR_NONE = 0x0000,
    QMI_ERR_MALFORMED_MSG = 0x0001,
    QMI_ERR_NO_MEMORY = 0x0002,
    QMI_ERR_INTERNAL = 0x0003,
    QMI_ERR_ABORTED = 0x0004,
    QMI_ERR_CLIENT_IDS_EXHAUSTED = 0x0005,
    QMI_ERR_UNABORTABLE_TRANSACTION = 0x0006,
    QMI_ERR_INVALID_CLIENT_ID = 0x0007,
    QMI_ERR_NO_THRESHOLDS = 0x0008,
    QMI_ERR_INVALID_HANDLE = 0x0009,
    QMI_ERR_INVALID_PROFILE = 0x000A,
    QMI_ERR_INVALID_PINID = 0x000B,
    QMI_ERR_INCORRECT_PIN = 0x000C,
    QMI_ERR_NO_NETWORK_FOUND = 0x000D,
    QMI_ERR_CALL_FAILED = 0x000E,
    QMI_ERR_OUT_OF_CALL = 0x000F,
    QMI_ERR_NOT_PROVISIONED = 0x0010,
    QMI_ERR_MISSING_ARG = 0x0011,
    QMI_ERR_ARG_TOO_LONG = 0x0013,
    QMI_ERR_INVALID_TX_ID = 0x0016,
    QMI_ERR_DEVICE_IN_USE = 0x0017,
    QMI_ERR_OP_NETWORK_UNSUPPORTED = 0x0018,
    QMI_ERR_OP_DEVICE_UNSUPPORTED = 0x0019,
    QMI_ERR_NO_EFFECT = 0x001A,
    QMI_ERR_NO_FREE_PROFILE = 0x001B,
    QMI_ERR_INVALID_PDP_TYPE = 0x001C,
    QMI_ERR_INVALID_TECH_PREF = 0x001D,
    QMI_ERR_INVALID_PROFILE_TYPE = 0x001E,
    QMI_ERR_INVALID_SERVICE_TYPE = 0x001F,
    QMI_ERR_INVALID_REGISTER_ACTION = 0x0020,
    QMI_ERR_INVALID_PS_ATTACH_ACTION = 0x0021,
    QMI_ERR_AUTHENTICATION_FAILED = 0x0022,
    QMI_ERR_PIN_BLOCKED = 0x0023,
    QMI_ERR_PIN_PERM_BLOCKED = 0x0024,
    QMI_ERR_UIM_NOT_INITIALIZED = 0x0025,
    QMI_ERR_INVALID_QMI_CMD = 0x0047,
    QMI_ERR_INFO_UNAVAILABLE = 0x004A,
    QMI_ERR_ACCESS_DENIED = 0x0052,
    QMI_ERR_HARDWARE_RESTRICTED = 0x0053
};

const char *qmi_error_str(uint16_t qmi_error); /* never NULL */

/* ------------------------------------------------------------------ */
/* Reader / writer primitives                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    const uint8_t *p;
    size_t n;
} qmi_span_t;

/* Bounds-checked little-endian reader with a sticky error flag. */
typedef struct {
    const uint8_t *p;
    size_t n;
    size_t off;
    int err;
} qmi_rd_t;

void qmi_rd_init(qmi_rd_t *r, const uint8_t *p, size_t n);
uint8_t qmi_rd_u8(qmi_rd_t *r);
uint16_t qmi_rd_u16(qmi_rd_t *r);
uint32_t qmi_rd_u32(qmi_rd_t *r);
uint64_t qmi_rd_u64(qmi_rd_t *r);
int8_t qmi_rd_i8(qmi_rd_t *r);
int16_t qmi_rd_i16(qmi_rd_t *r);
int32_t qmi_rd_i32(qmi_rd_t *r);
const uint8_t *qmi_rd_bytes(qmi_rd_t *r, size_t n); /* NULL and err on underrun */
size_t qmi_rd_remaining(const qmi_rd_t *r);
int qmi_rd_ok(const qmi_rd_t *r); /* 1 when no underrun happened */

/* Copy a (not necessarily NUL-terminated) wire string into dst. Always NUL
 * terminates dst (cap >= 1); truncates silently; returns the stored length. */
size_t qmi_copy_str(char *dst, size_t cap, const uint8_t *src, size_t srclen);

/* Bounds-checked writer with a sticky overflow flag. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int overflow;
} qmi_wbuf_t;

void qmi_wbuf_init(qmi_wbuf_t *w, uint8_t *buf, size_t cap);
int qmi_wbuf_ok(const qmi_wbuf_t *w); /* 1 when nothing overflowed */
int qmi_wbuf_u8(qmi_wbuf_t *w, uint8_t v);
int qmi_wbuf_u16(qmi_wbuf_t *w, uint16_t v);
int qmi_wbuf_u32(qmi_wbuf_t *w, uint32_t v);
int qmi_wbuf_u64(qmi_wbuf_t *w, uint64_t v);
int qmi_wbuf_bytes(qmi_wbuf_t *w, const void *p, size_t n);

/* Whole-TLV helpers. All return 0 on success, -1 when the buffer overflowed
 * (the writer stays poisoned; check qmi_wbuf_ok() once at the end). */
int qmi_tlv_put(qmi_wbuf_t *w, uint8_t type, const void *v, size_t len);
int qmi_tlv_put_u8(qmi_wbuf_t *w, uint8_t type, uint8_t v);
int qmi_tlv_put_u16(qmi_wbuf_t *w, uint8_t type, uint16_t v);
int qmi_tlv_put_u32(qmi_wbuf_t *w, uint8_t type, uint32_t v);
int qmi_tlv_put_u64(qmi_wbuf_t *w, uint8_t type, uint64_t v);
int qmi_tlv_put_str(qmi_wbuf_t *w, uint8_t type, const char *s); /* no NUL on wire */

/* Composite TLVs: begin, append body with qmi_wbuf_*, end patches the length. */
int qmi_tlv_begin(qmi_wbuf_t *w, uint8_t type, size_t *mark);
int qmi_tlv_end(qmi_wbuf_t *w, size_t mark);

/* ------------------------------------------------------------------ */
/* TLV navigation                                                      */
/* ------------------------------------------------------------------ */
/* Find the first TLV of 'type'. Returns 1 found (value filled), 0 absent,
 * -1 when the TLV region is malformed before the type was found. */
int qmi_tlv_find(qmi_span_t tlvs, uint8_t type, qmi_span_t *value);

/* Iterate: *off must start at 0. Returns 1 and fills type/value, 0 at the end
 * of the region, -1 on a truncated TLV. */
int qmi_tlv_next(qmi_span_t tlvs, size_t *off, uint8_t *type, qmi_span_t *value);

/* Checks that the region is an exact sequence of well formed TLVs. */
int qmi_tlv_validate(qmi_span_t tlvs); /* 0 ok, -1 malformed */

/* Result TLV (0x02) extracted from a response. */
typedef struct {
    int has_result;    /* TLV 0x02 present */
    uint16_t result;   /* QMI_RESULT_SUCCESS / QMI_RESULT_FAILURE */
    uint16_t error;    /* enum qmi_error_code */
    qmi_span_t tlvs;   /* the complete TLV region of the response */
} qmi_resp_t;

int qmi_resp_parse(qmi_span_t tlvs, qmi_resp_t *out); /* 0 ok, -1 malformed */
static inline int qmi_resp_ok(const qmi_resp_t *r)
{
    return r->has_result && r->result == QMI_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* QMUX frame encode / decode                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    QMI_MSG_REQUEST = 0,
    QMI_MSG_RESPONSE = 1,
    QMI_MSG_INDICATION = 2
} qmi_msg_type_t;

typedef struct {
    uint8_t service;
    uint8_t client_id;
    uint8_t qmux_flags; /* raw QMUX flags byte */
    uint8_t qmi_flags;  /* raw QMI SDU flags byte */
    qmi_msg_type_t type;
    uint16_t txid; /* CTL: 8 bit value */
    uint16_t msg_id;
    qmi_span_t tlvs; /* points into the decoded frame */
} qmi_msg_t;

/* Encodes one complete QMUX frame. Returns the number of bytes written or 0
 * on error (buffer too small / tlv_len too large). tlvs may be NULL when
 * tlv_len is 0. */
size_t qmi_msg_encode(uint8_t *out, size_t cap, uint8_t service, uint8_t client_id,
                      qmi_msg_type_t type, uint16_t txid, uint16_t msg_id,
                      const uint8_t *tlvs, size_t tlv_len);

/* Decodes exactly one complete frame. Returns 0 or a negative QMI_DEC_*. */
#define QMI_DEC_SHORT (-1)    /* frame shorter than its headers */
#define QMI_DEC_MARKER (-2)   /* first byte is not 0x01 */
#define QMI_DEC_LENGTH (-3)   /* QMUX length does not match the buffer */
#define QMI_DEC_TLVLEN (-4)   /* QMI tlv_len exceeds the frame */
int qmi_msg_decode(const uint8_t *frame, size_t len, qmi_msg_t *out);

/* ------------------------------------------------------------------ */
/* Stream de-framer (copes with split / coalesced reads and garbage)   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t buf[QMI_MAX_FRAME + 16];
    size_t len;     /* buffered bytes */
    size_t consume; /* bytes of the frame handed out by the last pop */
    unsigned long resyncs;
    unsigned long dropped_bytes;
} qmi_deframer_t;

void qmi_deframer_init(qmi_deframer_t *d);
/* Appends up to n bytes; returns how many were taken (< n only when full:
 * drain with qmi_deframer_pop and push the remainder). */
size_t qmi_deframer_push(qmi_deframer_t *d, const uint8_t *data, size_t n);
/* Returns 1 and fills *msg when a complete frame is available (msg->tlvs
 * points into the internal buffer and stays valid until the next push/pop),
 * 0 when more data is needed. Garbage is skipped and accounted for. */
int qmi_deframer_pop(qmi_deframer_t *d, qmi_msg_t *msg);

/* ------------------------------------------------------------------ */
/* Logging hook and hex helper                                         */
/* ------------------------------------------------------------------ */
enum { QMI_LOG_ERROR = 0, QMI_LOG_WARN = 1, QMI_LOG_INFO = 2, QMI_LOG_DEBUG = 3 };
typedef void (*qmi_log_fn)(int level, const char *msg);
void qmi_set_logger(qmi_log_fn fn, int max_level);
void qmi_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Writes "01:0B:00..." (uppercase, ':' separated) truncated to cap. */
void qmi_hex(char *dst, size_t cap, const uint8_t *p, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* QMI_CODEC_H */
