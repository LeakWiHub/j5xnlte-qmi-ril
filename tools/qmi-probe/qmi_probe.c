/* SPDX-License-Identifier: Apache-2.0
 *
 * qmi-probe: libc-free diagnostic for /dev/wwan0qmi0 (aarch64 Linux).
 * Verifies, in order, criteria 1-5 of the j5xnlte project:
 *   1 open the QMUX device        2 CTL answers (+ client allocation)
 *   3 DMS identifies the modem    4 UIM reports the SIM/card state
 *   5 NAS reports serving system + signal
 * It prints every raw frame, including the QMUX flags byte, so the real wire
 * format can be checked instead of assumed.  Read-only by default; the
 * option --online additionally sends DMS Set Operating Mode = online.
 *
 * usage: qmi-probe [--online] [--provision] [--quiet-raw] [/dev/wwan0qmi0]
 */
#include <stddef.h>
#include <stdint.h>

#include "qmi/qmi_codec.h"

/* ---------------- raw aarch64 syscalls ---------------- */
#define SYS_openat 56
#define SYS_close 57
#define SYS_read 63
#define SYS_write 64
#define SYS_ppoll 73
#define SYS_exit_group 94
#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_CLOEXEC 02000000
#define POLLIN 1

static long sc(long n, long a, long b, long c, long d, long e)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory", "cc");
    return x0;
}

/* compiler-required helpers (no libc) */
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = d;
    const uint8_t *b = s;
    if (a < b) {
        while (n--) *a++ = *b++;
    } else {
        a += n;
        b += n;
        while (n--) *--a = *--b;
    }
    return d;
}
void *memset(void *d, int c, size_t n)
{
    uint8_t *a = d;
    while (n--) *a++ = (uint8_t)c;
    return d;
}
size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ---------------- tiny output layer ---------------- */
static void out(const char *s) { sc(SYS_write, 1, (long)s, (long)strlen(s), 0, 0); }
static void outn(long v)
{
    char b[24];
    int i = 23;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (v < 0) b[--i] = '-';
    out(b + i);
}
static void outx(unsigned v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    char b[12];
    int i;
    b[digits] = 0;
    for (i = digits - 1; i >= 0; i--, v >>= 4) b[i] = hx[v & 15];
    out("0x");
    out(b);
}
static void outhex(const uint8_t *p, size_t n)
{
    char b[3 * 96 + 4];
    size_t k = n > 96 ? 96 : n;
    qmi_hex(b, sizeof(b), p, k);
    out(b);
    if (n > k) out(":...");
}

/* ---------------- state ---------------- */
static int g_fd = -1;
static int g_raw = 1;
static qmi_deframer_t g_df;
static uint16_t g_tx[32];
static int g_seen_qmux_flags = -1;
static uint8_t g_rsp[QMI_MAX_FRAME];
static size_t g_rsp_len;

static int g_provision;

static void nap(long ms)
{
    struct {
        long s, ns;
    } ts = {ms / 1000, (ms % 1000) * 1000000L};
    sc(SYS_ppoll, 0, 0, (long)&ts, 0, 8);
}

static void ppoll_in(int fd, long ms, int *ready)
{
    struct {
        int fd;
        short ev, rev;
    } pf = {fd, POLLIN, 0};
    struct {
        long s, ns;
    } ts = {ms / 1000, (ms % 1000) * 1000000L};
    long r = sc(SYS_ppoll, (long)&pf, 1, (long)&ts, 0, 8);
    *ready = (r > 0 && (pf.rev & POLLIN));
}

/* One request/response exchange. Returns 0 transport ok (resp parsed into *rs),
 * negative on error. Indications and foreign frames are logged and skipped. */
static int xfer(uint8_t svc, uint8_t cid, uint16_t msg, const uint8_t *tl, size_t tln, qmi_resp_t *rs, qmi_msg_t *mo)
{
    uint8_t f[512];
    uint16_t tx = ++g_tx[svc];
    long waited = 0;
    size_t n;
    if (svc == QMI_SVC_CTL) tx &= 0xFF;
    if (!tx) tx = ++g_tx[svc];
    n = qmi_msg_encode(f, sizeof(f), svc, cid, QMI_MSG_REQUEST, tx, msg, tl, tln);
    if (!n) return -1;
    if (g_raw) {
        out("    TX "); outhex(f, n); out("\n");
    }
    if (sc(SYS_write, g_fd, (long)f, (long)n, 0, 0) != (long)n) {
        out("    write failed\n");
        return -2;
    }
    while (waited < 3000) {
        int ready;
        qmi_msg_t m;
        ppoll_in(g_fd, 250, &ready);
        waited += 250;
        if (ready) {
            uint8_t rb[QMI_MAX_FRAME];
            long r = sc(SYS_read, g_fd, (long)rb, sizeof(rb), 0, 0);
            size_t off = 0;
            if (r <= 0) {
                out("    read failed: "); outn(r); out("\n");
                return -3;
            }
            if (g_raw) {
                out("    RX "); outhex(rb, (size_t)r); out("\n");
            }
            while (off < (size_t)r) off += qmi_deframer_push(&g_df, rb + off, (size_t)r - off);
        }
        while (qmi_deframer_pop(&g_df, &m)) {
            if (g_seen_qmux_flags < 0) g_seen_qmux_flags = m.qmux_flags;
            if (m.type == QMI_MSG_RESPONSE && m.service == svc && m.txid == tx && (svc == QMI_SVC_CTL || m.client_id == cid)) {
                *mo = m;
                if (m.tlvs.n > sizeof(g_rsp)) return -4;
                memcpy(g_rsp, m.tlvs.p, m.tlvs.n);
                g_rsp_len = m.tlvs.n;
                mo->tlvs.p = g_rsp;
                if (qmi_resp_parse(mo->tlvs, rs) < 0) {
                    out("    malformed TLVs in response\n");
                    return -5;
                }
                return 0;
            }
            out("    (skipped frame: svc="); out(qmi_service_name(m.service));
            out(m.type == QMI_MSG_INDICATION ? " indication msg=" : " other msg="); outx(m.msg_id, 4); out(")\n");
        }
    }
    out("    TIMEOUT (no response in 3 s)\n");
    return -6;
}

static void result_line(const char *name, int rc, const qmi_resp_t *rs)
{
    out(rc == 0 && qmi_resp_ok(rs) ? "  [OK]   " : "  [FAIL] ");
    out(name);
    if (rc) {
        out(": transport error "); outn(rc);
    } else if (!rs->has_result) {
        out(": response has no result TLV");
    } else if (rs->result != QMI_RESULT_SUCCESS) {
        out(": QMI error "); outx(rs->error, 4); out(" "); out(qmi_error_str(rs->error));
    }
    out("\n");
}

static int call(const char *name, uint8_t svc, uint8_t cid, uint16_t msg, const uint8_t *tl, size_t tln, qmi_msg_t *m, qmi_resp_t *rs)
{
    int rc = xfer(svc, cid, msg, tl, tln, rs, m);
    result_line(name, rc, rs);
    return rc == 0 && qmi_resp_ok(rs);
}

static int alloc_cid(uint8_t svc, uint8_t *cid)
{
    uint8_t tl[4];
    qmi_wbuf_t w;
    qmi_msg_t m;
    qmi_resp_t rs;
    qmi_span_t v;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put_u8(&w, 0x01, svc);
    if (!call("CTL allocate CID", QMI_SVC_CTL, 0, 0x0022, tl, w.len, &m, &rs)) return 0;
    if (qmi_tlv_find(m.tlvs, 0x01, &v) != 1 || v.n < 2 || v.p[0] != svc) {
        out("    allocate CID response lacks TLV 0x01\n");
        return 0;
    }
    *cid = v.p[1];
    out("         service "); out(qmi_service_name(svc)); out(" -> client id "); outn(*cid); out("\n");
    return 1;
}

static void release_cid(uint8_t svc, uint8_t cid)
{
    uint8_t tl[8], v[2];
    qmi_wbuf_t w;
    qmi_msg_t m;
    qmi_resp_t rs;
    v[0] = svc;
    v[1] = cid;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    qmi_tlv_put(&w, 0x01, v, 2);
    xfer(QMI_SVC_CTL, 0, 0x0023, tl, w.len, &rs, &m);
}

static void print_str_tlv(const char *label, qmi_span_t tl, uint8_t type)
{
    qmi_span_t v;
    char s[96];
    if (qmi_tlv_find(tl, type, &v) == 1) {
        qmi_copy_str(s, sizeof(s), v.p, v.n);
        out("         "); out(label); out(": "); out(s); out("\n");
    }
}

static const char *app_state(unsigned s)
{
    static const char *n[] = {"unknown", "detected", "pin1-or-upin-required", "puk1-required", "check-perso", "pin1-blocked", "illegal", "ready"};
    return s < 8 ? n[s] : "?";
}

static void parse_card_status(qmi_span_t v)
{
    qmi_rd_t r;
    unsigned slots, s;
    qmi_rd_init(&r, v.p, v.n);
    qmi_rd_u16(&r); qmi_rd_u16(&r); qmi_rd_u16(&r); qmi_rd_u16(&r); /* primary/secondary indices */
    slots = qmi_rd_u8(&r);
    for (s = 0; s < slots && qmi_rd_ok(&r); s++) {
        unsigned card = qmi_rd_u8(&r), upin = qmi_rd_u8(&r), apps, a, errc;
        qmi_rd_u8(&r); qmi_rd_u8(&r); /* upin/upuk retries */
        errc = qmi_rd_u8(&r);         /* card error code */
        apps = qmi_rd_u8(&r);
        if (!qmi_rd_ok(&r)) break;
        out("         slot "); outn(s); out(": card_state="); outn(card);
        out(card == 0 ? " (absent)" : card == 1 ? " (present)" : " (error)");
        if (card == 2) { out(" error_code="); outn(errc); }
        out(" upin_state="); outn(upin); out(" apps="); outn(apps); out("\n");
        for (a = 0; a < apps && qmi_rd_ok(&r); a++) {
            unsigned type = qmi_rd_u8(&r), st = qmi_rd_u8(&r), aidl;
            qmi_rd_u8(&r); qmi_rd_u8(&r); qmi_rd_u8(&r); qmi_rd_u8(&r); /* perso state/feature/retries/unblock */
            aidl = qmi_rd_u8(&r);
            qmi_rd_bytes(&r, aidl);
            qmi_rd_u8(&r);                                              /* univ pin */
            { unsigned p1 = qmi_rd_u8(&r); qmi_rd_u8(&r); qmi_rd_u8(&r); qmi_rd_u8(&r); qmi_rd_u8(&r); qmi_rd_u8(&r);
              if (qmi_rd_ok(&r)) {
                  out("           app "); outn(a); out(": type="); outn(type); out(type == 1 ? " (SIM)" : type == 2 ? " (USIM)" : "");
                  out(" state="); outn(st); out(" ("); out(app_state(st)); out(") pin1_state="); outn(p1); out("\n");
              } }
        }
    }
    if (!qmi_rd_ok(&r)) out("         (card status TLV shorter than expected: see raw hex above)\n");
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static void stage(int n, const char *what, int pass)
{
    out("STAGE "); outn(n); out(pass ? " PASS: " : " FAIL: "); out(what); out("\n\n");
}

static int run(int online, const char *path)
{
    uint8_t cid_dms = 0, cid_nas = 0, cid_uim = 0;
    qmi_msg_t m;
    qmi_resp_t rs;
    qmi_span_t v;
    int ok[6] = {0, 0, 0, 0, 0, 0}, i;

    out("qmi-probe: device "); out(path); out("\n\n");
    qmi_deframer_init(&g_df);

    /* 1 ---------------------------------------------------------------- */
    g_fd = (int)sc(SYS_openat, AT_FDCWD, (long)path, O_RDWR | O_CLOEXEC, 0, 0);
    if (g_fd < 0) {
        out("  open failed, errno="); outn(-g_fd);
        out(g_fd == -13 ? "  (EACCES: run as root, or the ueventd/SELinux rules are missing)\n" : g_fd == -2 ? "  (ENOENT: node does not exist)\n" : "\n");
    } else {
        out("  [OK]   open("); out(path); out(") fd="); outn(g_fd); out("\n");
        ok[1] = 1;
    }
    stage(1, "QMUX device opens", ok[1]);
    if (!ok[1]) goto summary;

    /* 2 ---------------------------------------------------------------- */
    out("-- CTL\n");
    if (call("CTL get version info", QMI_SVC_CTL, 0, 0x0021, NULL, 0, &m, &rs) && qmi_tlv_find(m.tlvs, 0x01, &v) == 1 && v.n >= 1) {
        unsigned n = v.p[0], k;
        out("         services reported by the modem: "); outn(n); out("\n");
        for (k = 0; k < n && 1 + (k + 1) * 5 <= v.n; k++) {
            out("           "); out(qmi_service_name(v.p[1 + k * 5])); out(" (");
            outx(v.p[1 + k * 5], 2); out(") v"); outn(v.p[2 + k * 5] | (v.p[3 + k * 5] << 8)); out("."); outn(v.p[4 + k * 5] | (v.p[5 + k * 5] << 8)); out("\n");
        }
    }
    ok[2] = alloc_cid(QMI_SVC_DMS, &cid_dms);
    if (ok[2]) {
        alloc_cid(QMI_SVC_NAS, &cid_nas);
        alloc_cid(QMI_SVC_UIM, &cid_uim);
    }
    if (g_seen_qmux_flags >= 0) {
        out("         QMUX flags byte seen on modem->host frames: "); outx((unsigned)g_seen_qmux_flags, 2);
        out(g_seen_qmux_flags == 0x80 ? " (as expected for QMUX)\n" : " (differs from the usual 0x80 - decoder ignores it, report this)\n");
    }
    stage(2, "CTL answers and allocates a DMS client id", ok[2]);
    if (!ok[2]) goto summary;

    /* 3 ---------------------------------------------------------------- */
    out("-- DMS\n");
    if (call("DMS get revision", QMI_SVC_DMS, cid_dms, 0x0023, NULL, 0, &m, &rs)) { print_str_tlv("revision", m.tlvs, 0x01); ok[3] = 1; }
    if (call("DMS get IDs", QMI_SVC_DMS, cid_dms, 0x0025, NULL, 0, &m, &rs)) {
        print_str_tlv("IMEI", m.tlvs, 0x11); print_str_tlv("ESN", m.tlvs, 0x10); print_str_tlv("MEID", m.tlvs, 0x12); print_str_tlv("IMEI SV", m.tlvs, 0x13);
        ok[3] = 1;
    }
    if (call("DMS get operating mode", QMI_SVC_DMS, cid_dms, 0x002D, NULL, 0, &m, &rs) && qmi_tlv_find(m.tlvs, 0x01, &v) == 1 && v.n >= 1) {
        out("         operating mode: "); outn(v.p[0]);
        out(v.p[0] == 0 ? " (online)\n" : v.p[0] == 1 ? " (low-power: radio is OFF, NAS will not register until set online)\n"
            : " (NOT online; per the libqmi table 5 would be 'shutting down', not verified. Re-run with --online)\n");
        if (online && v.p[0] != 0) {
            uint8_t tl[4];
            qmi_wbuf_t w;
            qmi_wbuf_init(&w, tl, sizeof(tl));
            qmi_tlv_put_u8(&w, 0x01, 0);
            if (call("DMS set operating mode = online (--online)", QMI_SVC_DMS, cid_dms, 0x002E, tl, w.len, &m, &rs)) {
                /* the modem needs time: re-read the mode once a second for up to 8 s and show when it changes */
                int k;
                for (k = 0; k < 8; k++) {
                    nap(1000);
                    if (call("DMS get operating mode (after --online)", QMI_SVC_DMS, cid_dms, 0x002D, NULL, 0, &m, &rs) &&
                        qmi_tlv_find(m.tlvs, 0x01, &v) == 1 && v.n >= 1) {
                        out("         operating mode now: "); outn(v.p[0]); out("\n");
                        if (v.p[0] == 0) break;
                    }
                }
            }
        }
    }
    stage(3, "DMS returns modem identity", ok[3]);

    /* 4 ---------------------------------------------------------------- */
    out("-- UIM\n");
    if (cid_uim) {
        if (call("UIM get card status", QMI_SVC_UIM, cid_uim, 0x002F, NULL, 0, &m, &rs)) {
            if (qmi_tlv_find(m.tlvs, 0x10, &v) == 1) { parse_card_status(v); ok[4] = 1; }
            else out("         no card-status TLV (0x10) in the response\n");
        }
        /* --provision: what every Android RIL does. Slot 0 only; changes modem state (a reboot is the expected way back). */
        if (g_provision && ok[4] && qmi_tlv_find(m.tlvs, 0x10, &v) == 1 && v.n >= 23 && v.p[0] == 0xFF && v.p[1] == 0xFF &&
            v.p[9] == 1 && v.p[14] >= 1 && v.p[22 - 1] <= 16 && 22 + (size_t)v.p[21] <= v.n) {
            uint8_t t1[2] = {0, 1}, t2[18], tl[40];
            size_t k, al = v.p[21];
            qmi_wbuf_t w;
            t2[0] = 1;
            t2[1] = (uint8_t)al;
            for (k = 0; k < al; k++) t2[2 + k] = v.p[22 + k];
            qmi_wbuf_init(&w, tl, sizeof(tl));
            qmi_tlv_put(&w, 0x01, t1, 2);
            qmi_tlv_put(&w, 0x10, t2, 2 + al);
            out("-- UIM provisioning (--provision): activating primary GW session for slot 1\n");
            if (call("UIM change provisioning session (0x38)", QMI_SVC_UIM, cid_uim, 0x0038, tl, w.len, &m, &rs)) {
                int kk;
                for (kk = 0; kk < 10; kk++) {
                    nap(1000);
                    if (call("UIM get card status (after provisioning)", QMI_SVC_UIM, cid_uim, 0x002F, NULL, 0, &m, &rs) &&
                        qmi_tlv_find(m.tlvs, 0x10, &v) == 1) {
                        parse_card_status(v);
                        if (v.n >= 17 && v.p[16] != 1) break; /* application state left "detected" */
                    }
                }
            }
        } else if (g_provision) out("         (--provision: nothing to do: card not present, already provisioned, or no application)\n");
    }
    stage(4, "UIM reports card status (inspect the slot lines: absent/present is data, not a failure)", ok[4]);

    /* 5 ---------------------------------------------------------------- */
    out("-- NAS\n");
    if (cid_nas) {
        /* a modem without service answers FAILURE/INFO_UNAVAILABLE (0x4A) but still carries TLV 0x01: that is an answer */
        int sv = call("NAS get serving system", QMI_SVC_NAS, cid_nas, 0x0024, NULL, 0, &m, &rs);
        if (!sv && rs.has_result && rs.error == 0x004A) { sv = 1; out("         (INFO_UNAVAILABLE = no service yet; counted as an answer)\n"); }
        if (sv && qmi_tlv_find(m.tlvs, 0x01, &v) == 1 && v.n >= 5) {
            static const char *reg[] = {"not registered", "registered", "searching", "denied", "unknown"};
            out("         registration: "); out(v.p[0] < 5 ? reg[v.p[0]] : "?");
            out(" cs_attach="); outn(v.p[1]); out(" ps_attach="); outn(v.p[2]); out(" radio_if_count="); outn(v.p[4]); out("\n");
            ok[5] = 1;
        }
        if (call("NAS get signal strength", QMI_SVC_NAS, cid_nas, 0x0020, NULL, 0, &m, &rs) && qmi_tlv_find(m.tlvs, 0x01, &v) == 1 && v.n >= 2) {
            out("         signal: "); outn((int8_t)v.p[0]); out(" dBm, radio_if="); outn(v.p[1]); out("\n");
        }
    }
    stage(5, "NAS answers (registered or not is reported above, it is not what this stage tests)", ok[5]);

    /* extras: read-only requests the HAL sends at start-up. Informational, they do not change the exit status.
     * Their RAW frames are what is needed to confirm the layouts the HAL assumes (VOICE is not verified at all). */
    out("-- extras (read-only, informational)\n");
    if (cid_nas) {
        call("NAS get signal info (0x4F)", QMI_SVC_NAS, cid_nas, 0x004F, NULL, 0, &m, &rs);
        call("NAS get system selection preference (0x34)", QMI_SVC_NAS, cid_nas, 0x0034, NULL, 0, &m, &rs);
    }
    {
        uint8_t cid_voice = 0, cid_wda = 0;
        if (alloc_cid(QMI_SVC_VOICE, &cid_voice)) {
            call("VOICE get all call info (0x2F)", QMI_SVC_VOICE, cid_voice, 0x002F, NULL, 0, &m, &rs);
            release_cid(QMI_SVC_VOICE, cid_voice);
        }
        if (alloc_cid(QMI_SVC_WDA, &cid_wda)) {
            call("WDA get data format (0x21)", QMI_SVC_WDA, cid_wda, 0x0021, NULL, 0, &m, &rs);
            release_cid(QMI_SVC_WDA, cid_wda);
        }
    }
    out("\n");

    release_cid(QMI_SVC_UIM, cid_uim);
    release_cid(QMI_SVC_NAS, cid_nas);
    release_cid(QMI_SVC_DMS, cid_dms);
    sc(SYS_close, g_fd, 0, 0, 0, 0);

summary:
    out("==== SUMMARY (stages 1-5 of the readiness list; stages 6-12 need the HAL) ====\n");
    for (i = 1; i <= 5; i++) { out("  stage "); outn(i); out(": "); out(ok[i] ? "PASS\n" : "FAIL/not reached\n"); }
    return ok[1] && ok[2] && ok[3] && ok[4] && ok[5] ? 0 : 1;
}

__attribute__((used)) void c_start(long *sp)
{
    int argc = (int)sp[0], online = 0, i;
    char **argv = (char **)(sp + 1);
    const char *path = "/dev/wwan0qmi0";
    int rc;
    for (i = 1; i < argc; i++) {
        if (streq(argv[i], "--online")) online = 1;
        else if (streq(argv[i], "--provision")) g_provision = 1;
        else if (streq(argv[i], "--quiet-raw")) g_raw = 0;
        else path = argv[i];
    }
    rc = run(online, path);
    sc(SYS_exit_group, rc, 0, 0, 0, 0);
    for (;;) {
    }
}

__asm__(".text\n.global _start\n_start:\n  mov x0, sp\n  and sp, x0, #-16\n  bl c_start\n");
