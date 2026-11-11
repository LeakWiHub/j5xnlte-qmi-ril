/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include "qmi/qmi_dev.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_PENDING 32
#define CTL_ALLOCATE_CID 0x0022
#define CTL_RELEASE_CID 0x0023

struct pend {
    int used;
    int state; /* 0 waiting, 1 done, 2 failed */
    int err;   /* negative errno when failed */
    uint8_t svc, cid;
    uint16_t txid;
    uint8_t *buf;
    size_t cap, len;
    int overflow;
};

struct qmi_dev {
    qmi_dev_cfg_t cfg;
    char path[160];

    pthread_mutex_t mu;  /* fd, up, stop, pending, counters, stats */
    pthread_cond_t cv;   /* CLOCK_MONOTONIC */
    pthread_mutex_t wmu; /* serialises write() */

    int fd;
    int up;
    int stop;
    int wake[2];
    pthread_t th;
    int th_started;

    struct pend pend[MAX_PENDING];
    uint16_t txctr[256];
    qmi_dev_stats_t st;
    qmi_deframer_t df; /* touched only by the reader thread */
};

/* ------------------------------------------------------------------ */
static void ts_after(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int default_open(void *ctx)
{
    qmi_dev_t *d = ctx;
    int fd = open(d->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

static int do_open(qmi_dev_t *d)
{
    int fd = d->cfg.open_fn ? d->cfg.open_fn(d->cfg.open_ctx) : default_open(d);
    if (fd >= 0) {
        int fl = fcntl(fd, F_GETFL);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    return fd;
}

/* mu must be held */
static void fail_all_locked(qmi_dev_t *d, int err)
{
    int i;
    for (i = 0; i < MAX_PENDING; i++)
        if (d->pend[i].used && d->pend[i].state == 0) {
            d->pend[i].state = 2;
            d->pend[i].err = err;
        }
    pthread_cond_broadcast(&d->cv);
}

static void set_down(qmi_dev_t *d)
{
    int was_up, fd;
    pthread_mutex_lock(&d->mu);
    fd = d->fd;
    d->fd = -1;
    was_up = d->up;
    d->up = 0;
    fail_all_locked(d, -ENOTCONN);
    pthread_mutex_unlock(&d->mu);
    if (fd >= 0) close(fd);
    if (was_up) {
        qmi_logf(QMI_LOG_WARN, "qmi: device down");
        if (d->cfg.on_state) d->cfg.on_state(d->cfg.cb_ctx, 0);
    }
}

static void set_up(qmi_dev_t *d, int fd, int reconnect)
{
    pthread_mutex_lock(&d->mu);
    d->fd = fd;
    d->up = 1;
    if (reconnect) d->st.reconnects++;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mu);
    qmi_deframer_init(&d->df);
    qmi_logf(QMI_LOG_INFO, "qmi: device up%s", reconnect ? " (reconnected)" : "");
    if (d->cfg.on_state) d->cfg.on_state(d->cfg.cb_ctx, 1);
}

/* ------------------------------------------------------------------ */
static void dispatch(qmi_dev_t *d, const qmi_msg_t *m)
{
    int i, matched = 0;
    if (m->type == QMI_MSG_INDICATION) {
        pthread_mutex_lock(&d->mu);
        d->st.rx_ind++;
        pthread_mutex_unlock(&d->mu);
        if (d->cfg.on_indication) d->cfg.on_indication(d->cfg.cb_ctx, m);
        return;
    }
    if (m->type != QMI_MSG_RESPONSE) {
        qmi_logf(QMI_LOG_WARN, "qmi: unexpected request-type frame from modem svc=%s msg=0x%04x",
                 qmi_service_name(m->service), m->msg_id);
        return;
    }
    pthread_mutex_lock(&d->mu);
    for (i = 0; i < MAX_PENDING; i++) {
        struct pend *p = &d->pend[i];
        if (p->used && p->state == 0 && p->svc == m->service && p->txid == m->txid &&
            (m->service == QMI_SVC_CTL || p->cid == m->client_id)) {
            if (p->buf) { /* caller wants the body */
                if (m->tlvs.n <= p->cap) {
                    if (m->tlvs.n) memcpy(p->buf, m->tlvs.p, m->tlvs.n);
                } else {
                    p->overflow = 1;
                }
            }
            p->len = m->tlvs.n;
            p->state = 1;
            matched = 1;
            d->st.rx_resp++;
            pthread_cond_broadcast(&d->cv);
            break;
        }
    }
    if (!matched) d->st.stray_resp++;
    pthread_mutex_unlock(&d->mu);
    if (!matched)
        qmi_logf(QMI_LOG_DEBUG, "qmi: stray response svc=%s cid=%u tx=%u msg=0x%04x (late or foreign)",
                 qmi_service_name(m->service), m->client_id, m->txid, m->msg_id);
}

static void pump(qmi_dev_t *d, const uint8_t *data, size_t n)
{
    size_t off = 0;
    qmi_msg_t m;
    while (off < n) {
        off += qmi_deframer_push(&d->df, data + off, n - off);
        while (qmi_deframer_pop(&d->df, &m)) dispatch(d, &m);
    }
    pthread_mutex_lock(&d->mu);
    d->st.resyncs = d->df.resyncs;
    d->st.dropped_bytes = d->df.dropped_bytes;
    pthread_mutex_unlock(&d->mu);
}

static void *reader_main(void *arg)
{
    qmi_dev_t *d = arg;
    uint8_t rb[QMI_MAX_FRAME];
    int backoff = d->cfg.reconnect_min_ms;

    for (;;) {
        struct pollfd pf[2];
        int fd, rc;

        pthread_mutex_lock(&d->mu);
        fd = d->fd;
        pthread_mutex_unlock(&d->mu);

        if (fd < 0) {
            if (!d->cfg.auto_reconnect) break;
            pf[0].fd = d->wake[0];
            pf[0].events = POLLIN;
            rc = poll(pf, 1, backoff);
            if (rc > 0) break; /* stop requested */
            if (rc < 0 && errno != EINTR) break;
            fd = do_open(d);
            if (fd < 0) {
                backoff = backoff * 2 > d->cfg.reconnect_max_ms ? d->cfg.reconnect_max_ms : backoff * 2;
                continue;
            }
            backoff = d->cfg.reconnect_min_ms;
            set_up(d, fd, 1);
            continue;
        }

        pf[0].fd = fd;
        pf[0].events = POLLIN;
        pf[1].fd = d->wake[0];
        pf[1].events = POLLIN;
        rc = poll(pf, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) continue;
            set_down(d);
            continue;
        }
        if (pf[1].revents) break;
        if (pf[0].revents & POLLIN) {
            ssize_t n = read(fd, rb, sizeof(rb));
            if (n > 0) {
                pump(d, rb, (size_t)n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            set_down(d); /* EOF or hard error */
            continue;
        }
        if (pf[0].revents & (POLLHUP | POLLERR | POLLNVAL)) set_down(d);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
qmi_dev_t *qmi_dev_create(const qmi_dev_cfg_t *cfg)
{
    pthread_condattr_t ca;
    qmi_dev_t *d;
    int fd;

    d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cfg = *cfg;
    if (cfg->path) {
        strncpy(d->path, cfg->path, sizeof(d->path) - 1);
        d->cfg.path = d->path;
    }
    if (d->cfg.reconnect_min_ms <= 0) d->cfg.reconnect_min_ms = 200;
    if (d->cfg.reconnect_max_ms < d->cfg.reconnect_min_ms) d->cfg.reconnect_max_ms = 5000;
    d->fd = -1;
    d->wake[0] = d->wake[1] = -1;
    pthread_mutex_init(&d->mu, NULL);
    pthread_mutex_init(&d->wmu, NULL);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&d->cv, &ca);
    pthread_condattr_destroy(&ca);
    qmi_deframer_init(&d->df);

    if (pipe2(d->wake, O_CLOEXEC | O_NONBLOCK) < 0) goto fail;
    fd = do_open(d);
    if (fd < 0 && !d->cfg.auto_reconnect) {
        errno = -fd;
        goto fail;
    }
    if (fd >= 0) set_up(d, fd, 0);
    if (pthread_create(&d->th, NULL, reader_main, d) != 0) goto fail;
    d->th_started = 1;
    return d;

fail: {
    int e = errno;
    if (d->fd >= 0) close(d->fd);
    if (d->wake[0] >= 0) close(d->wake[0]);
    if (d->wake[1] >= 0) close(d->wake[1]);
    free(d);
    errno = e;
    return NULL;
}
}

void qmi_dev_destroy(qmi_dev_t *d)
{
    char c = 'x';
    if (!d) return;
    pthread_mutex_lock(&d->mu);
    d->stop = 1;
    fail_all_locked(d, -ECANCELED);
    pthread_mutex_unlock(&d->mu);
    if (write(d->wake[1], &c, 1) < 0) { /* pipe full means already woken */ }
    if (d->th_started) pthread_join(d->th, NULL);
    if (d->fd >= 0) close(d->fd);
    close(d->wake[0]);
    close(d->wake[1]);
    pthread_cond_destroy(&d->cv);
    pthread_mutex_destroy(&d->mu);
    pthread_mutex_destroy(&d->wmu);
    free(d);
}

int qmi_dev_is_up(qmi_dev_t *d)
{
    int up;
    pthread_mutex_lock(&d->mu);
    up = d->up;
    pthread_mutex_unlock(&d->mu);
    return up;
}

int qmi_dev_wait_up(qmi_dev_t *d, int timeout_ms)
{
    struct timespec dl;
    int rc = 0;
    ts_after(&dl, timeout_ms);
    pthread_mutex_lock(&d->mu);
    while (!d->up && rc == 0) rc = pthread_cond_timedwait(&d->cv, &d->mu, &dl);
    rc = d->up ? 0 : -ETIMEDOUT;
    pthread_mutex_unlock(&d->mu);
    return rc;
}

void qmi_dev_get_stats(qmi_dev_t *d, qmi_dev_stats_t *out)
{
    pthread_mutex_lock(&d->mu);
    *out = d->st;
    pthread_mutex_unlock(&d->mu);
}

/* ------------------------------------------------------------------ */
static int write_all(qmi_dev_t *d, int fd, const uint8_t *p, size_t n)
{
    size_t off = 0;
    pthread_mutex_lock(&d->wmu);
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            struct pollfd pf = {.fd = fd, .events = POLLOUT};
            if (poll(&pf, 1, 2000) > 0) continue;
        }
        pthread_mutex_unlock(&d->wmu);
        return -EIO;
    }
    pthread_mutex_unlock(&d->wmu);
    return 0;
}

static int one_attempt(qmi_dev_t *d, uint8_t svc, uint8_t cid, uint16_t msg_id, const uint8_t *tlvs,
                       size_t tlv_len, int timeout_ms, uint8_t *rsp, size_t rsp_cap, size_t *rsp_len)
{
    uint8_t frame[QMI_MAX_FRAME];
    struct timespec dl;
    struct pend *p = NULL;
    uint16_t txid;
    size_t n;
    int i, fd, rc;

    pthread_mutex_lock(&d->mu);
    if (!d->up || d->stop) {
        pthread_mutex_unlock(&d->mu);
        return -ENOTCONN;
    }
    for (i = 0; i < MAX_PENDING && d->pend[i].used; i++) {
    }
    if (i == MAX_PENDING) {
        pthread_mutex_unlock(&d->mu);
        return -EBUSY;
    }
    p = &d->pend[i];
    /* unique, non-zero txid for this (service,client) among in-flight requests */
    for (;;) {
        int j, clash = 0;
        uint16_t t = ++d->txctr[svc];
        if (svc == QMI_SVC_CTL) t &= 0xFF;
        if (t == 0) continue;
        for (j = 0; j < MAX_PENDING; j++)
            if (d->pend[j].used && d->pend[j].svc == svc && d->pend[j].cid == cid && d->pend[j].txid == t) clash = 1;
        if (!clash) {
            txid = t;
            break;
        }
    }
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->svc = svc;
    p->cid = cid;
    p->txid = txid;
    p->buf = rsp;
    p->cap = rsp ? rsp_cap : 0;
    fd = d->fd;
    d->st.tx++;
    pthread_mutex_unlock(&d->mu);

    n = qmi_msg_encode(frame, sizeof(frame), svc, cid, QMI_MSG_REQUEST, txid, msg_id, tlvs, tlv_len);
    rc = n ? write_all(d, fd, frame, n) : -EMSGSIZE;
    if (rc) {
        pthread_mutex_lock(&d->mu);
        p->used = 0;
        pthread_mutex_unlock(&d->mu);
        return rc;
    }

    ts_after(&dl, timeout_ms);
    pthread_mutex_lock(&d->mu);
    while (p->state == 0) {
        int w = pthread_cond_timedwait(&d->cv, &d->mu, &dl);
        if (w == ETIMEDOUT && p->state == 0) break;
    }
    if (p->state == 0) {
        d->st.timeouts++;
        rc = -ETIMEDOUT;
    } else if (p->state == 2) {
        rc = p->err;
    } else if (p->overflow) {
        rc = -EMSGSIZE;
    } else {
        if (rsp_len) *rsp_len = p->len;
        rc = 0;
    }
    p->used = 0;
    pthread_mutex_unlock(&d->mu);
    return rc;
}

int qmi_dev_call(qmi_dev_t *d, uint8_t service, uint8_t client_id, uint16_t msg_id,
                 const uint8_t *tlvs, size_t tlv_len, int timeout_ms, int retries, uint8_t *rsp,
                 size_t rsp_cap, size_t *rsp_len)
{
    int attempt, rc = -ETIMEDOUT;
    if (rsp_len) *rsp_len = 0;
    for (attempt = 0; attempt <= retries; attempt++) {
        rc = one_attempt(d, service, client_id, msg_id, tlvs, tlv_len, timeout_ms, rsp, rsp_cap, rsp_len);
        if (rc != -ETIMEDOUT) break;
        qmi_logf(QMI_LOG_WARN, "qmi: timeout svc=%s msg=0x%04x (attempt %d/%d)", qmi_service_name(service),
                 msg_id, attempt + 1, retries + 1);
    }
    return rc;
}

int qmi_dev_call_ok(qmi_dev_t *d, uint8_t service, uint8_t client_id, uint16_t msg_id,
                    const uint8_t *tlvs, size_t tlv_len, int timeout_ms, int retries, uint8_t *rsp,
                    size_t rsp_cap, size_t *rsp_len)
{
    qmi_resp_t r;
    qmi_span_t sp;
    size_t len = 0;
    int rc;
    if (!rsp) return -EINVAL; /* the result TLV has to be parsed from somewhere */
    rc = qmi_dev_call(d, service, client_id, msg_id, tlvs, tlv_len, timeout_ms, retries, rsp, rsp_cap, &len);
    if (rsp_len) *rsp_len = len;
    if (rc) return rc;
    sp.p = rsp;
    sp.n = len;
    if (qmi_resp_parse(sp, &r) < 0 || !r.has_result) return -EPROTO;
    if (r.result != QMI_RESULT_SUCCESS) return -(QMI_E_BASE + (int)r.error);
    return 0;
}

int qmi_dev_alloc_client(qmi_dev_t *d, uint8_t service, uint8_t *client_id, int timeout_ms)
{
    uint8_t req[8], rsp[64];
    qmi_wbuf_t w;
    qmi_span_t sp, v;
    size_t len = 0;
    int rc;
    qmi_wbuf_init(&w, req, sizeof(req));
    qmi_tlv_put_u8(&w, 0x01, service);
    rc = qmi_dev_call_ok(d, QMI_SVC_CTL, 0, CTL_ALLOCATE_CID, req, w.len, timeout_ms, 1, rsp, sizeof(rsp), &len);
    if (rc) return rc;
    sp.p = rsp;
    sp.n = len;
    if (qmi_tlv_find(sp, 0x01, &v) != 1 || v.n < 2 || v.p[0] != service) return -EPROTO;
    *client_id = v.p[1];
    return 0;
}

int qmi_dev_release_client(qmi_dev_t *d, uint8_t service, uint8_t client_id, int timeout_ms)
{
    uint8_t req[8], rsp[64];
    uint8_t v[2] = {service, client_id};
    qmi_wbuf_t w;
    qmi_wbuf_init(&w, req, sizeof(req));
    qmi_tlv_put(&w, 0x01, v, 2);
    return qmi_dev_call_ok(d, QMI_SVC_CTL, 0, CTL_RELEASE_CID, req, w.len, timeout_ms, 0, rsp, sizeof(rsp), NULL);
}
