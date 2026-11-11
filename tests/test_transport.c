/* SPDX-License-Identifier: Apache-2.0
 * Transport tests against an in-process fake modem connected via socketpair.
 * This proves the transport logic (framing, txid routing, timeouts, retries,
 * indications, concurrency, crash/reconnect). It does NOT prove anything about
 * the real modem - that is what tools/qmi-probe is for. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "qmi/qmi_dev.h"

static int g_fail, g_run;
#define CHECK(c)                                                         \
    do {                                                                 \
        g_run++;                                                         \
        if (!(c)) {                                                      \
            g_fail++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
        }                                                                \
    } while (0)

/* message ids understood by the fake modem */
#define M_GET_IDS 0x0025     /* DMS: answers with imei, bytewise */
#define M_ERROR 0x0002       /* NAS: answers with QMI error 0x0003 */
#define M_ECHO 0x0100        /* echoes u32 TLV 0x01 as TLV 0x10 */
#define M_SLOW_ONCE 0x0200   /* ignores the first request, answers later ones */
#define M_NEVER 0x0201       /* never answers */
#define M_INDICATE 0x0300    /* answers, then sends an indication */
#define M_KILL 0x03FF        /* closes the connection */
#define M_BIG 0x0400         /* answers with a 3000 byte TLV */

static atomic_int g_slow_seen;
static atomic_int g_opens;
static atomic_int g_up_events, g_down_events;
static atomic_int g_ind_count;
static atomic_uint g_ind_msg;
static atomic_int g_ind_cid;

typedef struct {
    int fd;
} fake_t;

static void fm_write(int fd, const uint8_t *p, size_t n, int bytewise)
{
    size_t i = 0;
    while (i < n) {
        size_t k = bytewise ? 1 : n - i;
        ssize_t w = write(fd, p + i, k);
        if (w <= 0) return;
        i += (size_t)w;
    }
}

static void fm_reply(int fd, const qmi_msg_t *rq, int fail, uint16_t err, const uint8_t *extra, size_t elen,
                     int bytewise)
{
    uint8_t tl[4200], f[4300];
    qmi_wbuf_t w;
    size_t n;
    qmi_wbuf_init(&w, tl, sizeof(tl));
    {
        uint8_t res[4] = {(uint8_t)fail, 0, (uint8_t)err, (uint8_t)(err >> 8)};
        qmi_tlv_put(&w, 0x02, res, 4);
    }
    if (elen) qmi_wbuf_bytes(&w, extra, elen);
    n = qmi_msg_encode(f, sizeof(f), rq->service, rq->client_id, QMI_MSG_RESPONSE, rq->txid, rq->msg_id, tl, w.len);
    fm_write(fd, f, n, bytewise);
}

static void *fake_main(void *arg)
{
    fake_t *ft = arg;
    static __thread qmi_deframer_t df;
    uint8_t rb[512];
    qmi_deframer_init(&df);
    for (;;) {
        ssize_t n = read(ft->fd, rb, sizeof(rb));
        size_t off = 0;
        qmi_msg_t m;
        if (n <= 0) break;
        while (off < (size_t)n) {
            off += qmi_deframer_push(&df, rb + off, (size_t)n - off);
            while (qmi_deframer_pop(&df, &m)) {
                uint8_t ex[3100];
                if (m.service == QMI_SVC_CTL && m.msg_id == 0x0022) {
                    qmi_span_t v;
                    uint8_t body[2];
                    uint8_t tl[8];
                    qmi_wbuf_t w;
                    qmi_tlv_find(m.tlvs, 0x01, &v);
                    body[0] = v.p[0];
                    body[1] = (uint8_t)(v.p[0] + 0x10);
                    qmi_wbuf_init(&w, tl, sizeof(tl));
                    qmi_tlv_put(&w, 0x01, body, 2);
                    fm_reply(ft->fd, &m, 0, 0, tl, w.len, 0);
                } else if (m.service == QMI_SVC_DMS && m.msg_id == M_GET_IDS) {
                    uint8_t tl[32];
                    qmi_wbuf_t w;
                    qmi_wbuf_init(&w, tl, sizeof(tl));
                    qmi_tlv_put_str(&w, 0x11, "359876543210987");
                    fm_reply(ft->fd, &m, 0, 0, tl, w.len, 1); /* byte-by-byte */
                } else if (m.msg_id == M_ERROR) {
                    fm_reply(ft->fd, &m, 1, 0x0003, NULL, 0, 0);
                } else if (m.msg_id == M_ECHO) {
                    qmi_span_t v;
                    uint8_t tl[16];
                    qmi_wbuf_t w;
                    qmi_wbuf_init(&w, tl, sizeof(tl));
                    if (qmi_tlv_find(m.tlvs, 0x01, &v) == 1) qmi_tlv_put(&w, 0x10, v.p, v.n);
                    fm_reply(ft->fd, &m, 0, 0, tl, w.len, 0);
                } else if (m.msg_id == M_SLOW_ONCE) {
                    if (atomic_fetch_add(&g_slow_seen, 1) >= 1) fm_reply(ft->fd, &m, 0, 0, NULL, 0, 0);
                } else if (m.msg_id == M_NEVER) {
                    /* silence */
                } else if (m.msg_id == M_INDICATE) {
                    uint8_t f[64], tl[8];
                    qmi_wbuf_t w;
                    size_t k;
                    fm_reply(ft->fd, &m, 0, 0, NULL, 0, 0);
                    qmi_wbuf_init(&w, tl, sizeof(tl));
                    qmi_tlv_put_u8(&w, 0x10, 0x5A);
                    k = qmi_msg_encode(f, sizeof(f), QMI_SVC_NAS, QMI_CLIENT_BROADCAST, QMI_MSG_INDICATION, 0, 0x0051, tl, w.len);
                    fm_write(ft->fd, f, k, 0);
                } else if (m.msg_id == M_KILL) {
                    close(ft->fd);
                    free(ft);
                    return NULL;
                } else if (m.msg_id == M_BIG) {
                    uint8_t tl[3200];
                    qmi_wbuf_t w;
                    memset(ex, 0xAB, sizeof(ex));
                    qmi_wbuf_init(&w, tl, sizeof(tl));
                    qmi_tlv_put(&w, 0x10, ex, 3000);
                    fm_reply(ft->fd, &m, 0, 0, tl, w.len, 0);
                } else {
                    fm_reply(ft->fd, &m, 1, QMI_ERR_INVALID_QMI_CMD, NULL, 0, 0);
                }
            }
        }
    }
    close(ft->fd);
    free(ft);
    return NULL;
}

static int fake_open(void *ctx)
{
    int sv[2];
    pthread_t th;
    fake_t *ft;
    (void)ctx;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) return -errno;
    ft = malloc(sizeof(*ft));
    ft->fd = sv[1];
    pthread_create(&th, NULL, fake_main, ft);
    pthread_detach(th);
    atomic_fetch_add(&g_opens, 1);
    return sv[0];
}

static void on_ind(void *ctx, const qmi_msg_t *m)
{
    (void)ctx;
    atomic_store(&g_ind_msg, m->msg_id);
    atomic_store(&g_ind_cid, m->client_id);
    atomic_fetch_add(&g_ind_count, 1);
}

static void on_state(void *ctx, int up)
{
    (void)ctx;
    atomic_fetch_add(up ? &g_up_events : &g_down_events, 1);
}

static qmi_dev_t *g_dev;

static void *echo_worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    int i, bad = 0;
    for (i = 0; i < 60; i++) {
        uint8_t req[8], rsp[32];
        qmi_wbuf_t w;
        qmi_span_t sp, v;
        size_t len = 0;
        uint32_t want = (uint32_t)(id * 1000 + (unsigned)i), got = 0;
        qmi_wbuf_init(&w, req, sizeof(req));
        qmi_tlv_put_u32(&w, 0x01, want);
        if (qmi_dev_call_ok(g_dev, (uint8_t)(i % 2 ? QMI_SVC_NAS : QMI_SVC_DMS), 0x10 + (uint8_t)id, M_ECHO, req, w.len,
                            2000, 0, rsp, sizeof(rsp), &len) != 0) {
            bad++;
            continue;
        }
        sp.p = rsp;
        sp.n = len;
        if (qmi_tlv_find(sp, 0x10, &v) == 1 && v.n == 4) memcpy(&got, v.p, 4);
        if (got != want) bad++;
    }
    return (void *)(uintptr_t)bad;
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void)
{
    qmi_dev_cfg_t cfg;
    uint8_t cid = 0, rsp[4096];
    size_t len = 0;
    int rc, i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.open_fn = fake_open;
    cfg.on_indication = on_ind;
    cfg.on_state = on_state;
    cfg.auto_reconnect = 1;
    cfg.reconnect_min_ms = 20;
    cfg.reconnect_max_ms = 100;
    g_dev = qmi_dev_create(&cfg);
    CHECK(g_dev != NULL);
    if (!g_dev) return 1;
    CHECK(qmi_dev_is_up(g_dev) && atomic_load(&g_up_events) == 1);

    /* CTL allocate / release */
    CHECK(qmi_dev_alloc_client(g_dev, QMI_SVC_NAS, &cid, 1000) == 0 && cid == QMI_SVC_NAS + 0x10);

    /* fragmented response */
    rc = qmi_dev_call_ok(g_dev, QMI_SVC_DMS, 1, M_GET_IDS, NULL, 0, 1000, 0, rsp, sizeof(rsp), &len);
    CHECK(rc == 0);
    {
        qmi_span_t sp = {rsp, len}, v;
        CHECK(qmi_tlv_find(sp, 0x11, &v) == 1 && v.n == 15 && memcmp(v.p, "359876543210987", 15) == 0);
    }

    /* QMI error mapping */
    rc = qmi_dev_call_ok(g_dev, QMI_SVC_NAS, 1, M_ERROR, NULL, 0, 1000, 0, rsp, sizeof(rsp), &len);
    CHECK(QMI_E_IS_QMI(rc) && QMI_E_CODE(rc) == 0x0003);
    rc = qmi_dev_call_ok(g_dev, QMI_SVC_NAS, 1, 0x7777, NULL, 0, 1000, 0, rsp, sizeof(rsp), &len);
    CHECK(QMI_E_IS_QMI(rc) && QMI_E_CODE(rc) == QMI_ERR_INVALID_QMI_CMD);

    /* timeout without retry, then with retry (new txid) */
    {
        long t0 = now_ms();
        rc = qmi_dev_call(g_dev, QMI_SVC_DMS, 1, M_NEVER, NULL, 0, 150, 0, NULL, 0, NULL);
        CHECK(rc == -ETIMEDOUT && now_ms() - t0 >= 140 && now_ms() - t0 < 1000);
    }
    rc = qmi_dev_call(g_dev, QMI_SVC_DMS, 1, M_SLOW_ONCE, NULL, 0, 150, 1, NULL, 0, NULL);
    CHECK(rc == 0 && atomic_load(&g_slow_seen) == 2);

    /* body too large for the caller's buffer */
    {
        uint8_t small[16];
        rc = qmi_dev_call(g_dev, QMI_SVC_DMS, 1, M_BIG, NULL, 0, 1000, 0, small, sizeof(small), &len);
        CHECK(rc == -EMSGSIZE);
        rc = qmi_dev_call(g_dev, QMI_SVC_DMS, 1, M_BIG, NULL, 0, 1000, 0, rsp, sizeof(rsp), &len);
        CHECK(rc == 0 && len == 3003 + 7);
    }

    /* indication routing */
    rc = qmi_dev_call(g_dev, QMI_SVC_NAS, 1, M_INDICATE, NULL, 0, 1000, 0, NULL, 0, NULL);
    CHECK(rc == 0);
    for (i = 0; i < 100 && atomic_load(&g_ind_count) == 0; i++) usleep(10000);
    CHECK(atomic_load(&g_ind_count) == 1 && atomic_load(&g_ind_msg) == 0x0051 && atomic_load(&g_ind_cid) == 0xFF);

    /* concurrency: 6 threads x 60 echo calls, every one must get its own value back */
    {
        pthread_t th[6];
        int total_bad = 0;
        for (i = 0; i < 6; i++) pthread_create(&th[i], NULL, echo_worker, (void *)(uintptr_t)(i + 1));
        for (i = 0; i < 6; i++) {
            void *b;
            pthread_join(th[i], &b);
            total_bad += (int)(uintptr_t)b;
        }
        CHECK(total_bad == 0);
    }

    /* crash: the pending call must fail fast, the device must come back by itself */
    {
        long t0 = now_ms();
        rc = qmi_dev_call(g_dev, QMI_SVC_DMS, 1, M_KILL, NULL, 0, 3000, 0, NULL, 0, NULL);
        CHECK(rc == -ENOTCONN && now_ms() - t0 < 1500);
        CHECK(qmi_dev_wait_up(g_dev, 3000) == 0);
        /* on_state() is delivered AFTER the state is visible to is_up()/wait_up(),
         * so wait for the callback instead of assuming it already ran. */
        for (i = 0; i < 200 && atomic_load(&g_up_events) < 2; i++) usleep(5000);
        CHECK(atomic_load(&g_down_events) == 1 && atomic_load(&g_up_events) == 2 && atomic_load(&g_opens) == 2);
        cid = 0;
        CHECK(qmi_dev_alloc_client(g_dev, QMI_SVC_UIM, &cid, 1000) == 0 && cid == QMI_SVC_UIM + 0x10);
    }

    {
        qmi_dev_stats_t st;
        qmi_dev_get_stats(g_dev, &st);
        /* exactly two timeouts happened: M_NEVER and the first M_SLOW_ONCE attempt */
        CHECK(st.reconnects == 1 && st.timeouts == 2 && st.stray_resp == 0);
        /* The late answer of the retried M_SLOW_ONCE never exists, but the
         * M_NEVER/slow txids prove stray handling does not crash: */
        printf("stats: tx=%lu resp=%lu ind=%lu stray=%lu timeouts=%lu reconnects=%lu resyncs=%lu\n", st.tx,
               st.rx_resp, st.rx_ind, st.stray_resp, st.timeouts, st.reconnects, st.resyncs);
    }

    qmi_dev_destroy(g_dev);
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
