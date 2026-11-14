/* SPDX-License-Identifier: Apache-2.0
 * ril_core.c - lifecycle, client allocation, event dispatch, periodic signal polling. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ril_internal.h"

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

void ril_ev_push(struct ril_qmi *r, int type, int arg, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&r->mu);
    if (r->qn == RIL_EVQ) {
        /* overflow: drop the oldest event rather than blocking the QMI reader */
        r->qh = (r->qh + 1) % RIL_EVQ;
        r->qn--;
        r->dropped_events++;
    }
    {
        struct ril_ev_item *it = &r->q[(r->qh + r->qn) % RIL_EVQ];
        it->type = type;
        it->arg = arg;
        it->len = len > sizeof(it->data) ? sizeof(it->data) : len;
        if (it->len) memcpy(it->data, data, it->len);
        r->qn++;
    }
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
}

int ril_svc(struct ril_qmi *r, uint8_t svc, uint16_t msg, const uint8_t *tlvs, size_t n, int timeout_ms,
            int retries, uint8_t *rsp, size_t cap, size_t *rlen)
{
    uint8_t cid, tmp[QMI_MAX_FRAME];
    size_t len = 0;
    int rc;
    pthread_mutex_lock(&r->mu);
    cid = svc < sizeof(r->cid) ? r->cid[svc] : 0;
    pthread_mutex_unlock(&r->mu);
    if (!cid) return r->ready ? -ENOTSUP : -ENOTCONN;
    if (!rsp) {
        rsp = tmp;
        cap = sizeof(tmp);
    }
    rc = qmi_dev_call_ok(r->dev, svc, cid, msg, tlvs, n, timeout_ms ? timeout_ms : RIL_SVC_TIMEOUT_MS, retries,
                         rsp, cap, &len);
    if (rlen) *rlen = len;
    return rc;
}

/* ------------------------------------------------------------------ */
/* indications arrive on the QMI reader thread: translate and enqueue only */
static void on_indication(void *ctx, const qmi_msg_t *m)
{
    struct ril_qmi *r = ctx;
    switch (m->service) {
    case QMI_SVC_NAS: ril_nas_on_indication(r, m); break;
    case QMI_SVC_UIM: ril_uim_on_indication(r, m); break;
    case QMI_SVC_WMS: ril_wms_on_indication(r, m); break;
    case QMI_SVC_VOICE: ril_voice_on_indication(r, m); break;
    case QMI_SVC_WDS: ril_wds_on_indication(r, m); break;
    default:
        qmi_logf(QMI_LOG_DEBUG, "ril: ignored indication svc=%s msg=0x%04x", qmi_service_name(m->service), m->msg_id);
    }
}

static void on_state(void *ctx, int up)
{
    struct ril_qmi *r = ctx;
    pthread_mutex_lock(&r->mu);
    if (up) {
        r->need_init = 1;
    } else {
        r->need_init = 0;
        if (r->ready) {
            r->ready = 0;
            memset(r->cid, 0, sizeof(r->cid));
        }
    }
    pthread_mutex_unlock(&r->mu);
    ril_ev_push(r, up ? EV_NET : EV_NOT_READY, 0, NULL, 0); /* EV_NET on up is harmless and wakes the worker */
}

/* ------------------------------------------------------------------ */
static int init_clients(struct ril_qmi *r)
{
    static const uint8_t required[] = {QMI_SVC_DMS, QMI_SVC_NAS, QMI_SVC_UIM};
    static const uint8_t optional[] = {QMI_SVC_WMS, QMI_SVC_VOICE, QMI_SVC_WDS, QMI_SVC_WDA};
    size_t i;
    int rc;
    memset(r->cid, 0, sizeof(r->cid));
    for (i = 0; i < sizeof(required); i++) {
        uint8_t cid = 0;
        rc = qmi_dev_alloc_client(r->dev, required[i], &cid, 3000);
        if (rc) {
            qmi_logf(QMI_LOG_ERROR, "ril: cannot allocate required service %s: %d", qmi_service_name(required[i]), rc);
            return rc;
        }
        pthread_mutex_lock(&r->mu);
        r->cid[required[i]] = cid;
        pthread_mutex_unlock(&r->mu);
    }
    for (i = 0; i < sizeof(optional); i++) {
        uint8_t cid = 0;
        rc = qmi_dev_alloc_client(r->dev, optional[i], &cid, 3000);
        if (rc) {
            qmi_logf(QMI_LOG_WARN, "ril: optional service %s unavailable (%d)", qmi_service_name(optional[i]), rc);
            continue;
        }
        pthread_mutex_lock(&r->mu);
        r->cid[optional[i]] = cid;
        pthread_mutex_unlock(&r->mu);
    }
    return 0;
}

/* Android RILs bind the SIM application to the primary provisioning session themselves; without it the real modem
 * keeps the USIM "detected" (card-status index 0xFFFF) and never registers. Runs on the worker thread. */
static void sim_autoprovision(struct ril_qmi *r)
{
    ril_card_t c;
    if (ril_sim_status(r, &c) != 0 || !c.present || !c.has_app) { r->prov_tries = 0; return; }
    if (c.provisioned) { r->prov_tries = 0; return; }
    if (r->prov_tries >= 3) return; /* a failing request must not loop through its own indications */
    r->prov_tries++;
    {
        int rc = ril_sim_provision(r);
        if (rc) qmi_logf(QMI_LOG_WARN, "ril: UIM provisioning attempt %d failed (%d)", r->prov_tries, rc);
        else qmi_logf(QMI_LOG_INFO, "ril: UIM primary provisioning requested for slot %d app type %d", c.slot + 1, c.app_type);
    }
}

static void dispatch_event(struct ril_qmi *r, const struct ril_ev_item *e)
{
    const ril_events_t *ev = &r->ev;
    switch (e->type) {
    case EV_READY:
    case EV_NOT_READY:
        if (e->type == EV_READY) sim_autoprovision(r);
        if (ev->on_modem_state) ev->on_modem_state(ev->ctx, e->type == EV_READY);
        break;
    case EV_SIM:
        sim_autoprovision(r);
        /* the modem may have refused the SMS routes while the SIM was not ready: apply them again now */
        if (r->ready) ril_wms_register(r);
        if (ev->on_sim_changed) ev->on_sim_changed(ev->ctx);
        break;
    case EV_NET:
        if (r->ready) {
            static time_t last_wms;
            time_t now = time(NULL);
            if (now - last_wms >= 60) { last_wms = now; ril_wms_register(r); }
        }
        if (ev->on_network_changed && r->ready) ev->on_network_changed(ev->ctx);
        break;
    case EV_SMS_STORED:
        ril_wms_read_stored(r, e->data, e->len);
        break;
    case EV_SIG: if (ev->on_signal_changed) ev->on_signal_changed(ev->ctx); break;
    case EV_SMS:
        /* data = u32 LE modem transaction id, then SMSC + TPDU */
        if (e->len > 4) {
            uint32_t txid = (uint32_t)e->data[0] | ((uint32_t)e->data[1] << 8) | ((uint32_t)e->data[2] << 16) |
                            ((uint32_t)e->data[3] << 24);
            pthread_mutex_lock(&r->mu);
            r->sms_txid = txid;
            r->sms_ack_pending = e->arg;
            pthread_mutex_unlock(&r->mu);
            if (ev->on_sms) ev->on_sms(ev->ctx, e->data + 4, e->len - 4, e->arg);
        }
        break;
    case EV_CALLS: if (ev->on_calls_changed) ev->on_calls_changed(ev->ctx); break;
    case EV_DATA: if (ev->on_data_status) ev->on_data_status(ev->ctx, e->arg); break;
    }
}

static int sig_changed(const ril_signal_t *a, const ril_signal_t *b)
{
    return a->gsm_valid != b->gsm_valid || a->gsm_rssi != b->gsm_rssi || a->wcdma_valid != b->wcdma_valid ||
           a->wcdma_rssi != b->wcdma_rssi || a->lte_valid != b->lte_valid || a->lte_rssi != b->lte_rssi ||
           a->lte_rsrp != b->lte_rsrp || a->generic_valid != b->generic_valid || a->generic_dbm != b->generic_dbm;
}

static void *worker_main(void *arg)
{
    struct ril_qmi *r = arg;
    struct timespec next_poll;
    ts_after(&next_poll, 10000);
    for (;;) {
        struct ril_ev_item ev;
        int have = 0, do_init = 0, do_poll = 0;
        struct timespec now;

        pthread_mutex_lock(&r->mu);
        while (!r->stop && r->qn == 0 && !r->need_init) {
            int w = pthread_cond_timedwait(&r->cv, &r->mu, &next_poll);
            if (w == ETIMEDOUT) break;
        }
        if (r->stop) {
            pthread_mutex_unlock(&r->mu);
            return NULL;
        }
        if (r->need_init) {
            r->need_init = 0;
            do_init = 1;
        } else if (r->qn) {
            ev = r->q[r->qh];
            r->qh = (r->qh + 1) % RIL_EVQ;
            r->qn--;
            have = 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next_poll.tv_sec || (now.tv_sec == next_poll.tv_sec && now.tv_nsec >= next_poll.tv_nsec)) do_poll = 1;
        pthread_mutex_unlock(&r->mu);

        if (do_init) {
            if (init_clients(r) == 0) {
                ril_nas_register(r);
                ril_uim_register(r);
                ril_wms_register(r);
                ril_voice_register(r);
                ril_wda_setup(r);
                pthread_mutex_lock(&r->mu);
                r->ready = 1;
                pthread_cond_broadcast(&r->cv);
                pthread_mutex_unlock(&r->mu);
                qmi_logf(QMI_LOG_INFO, "ril: modem ready");
                ril_ev_push(r, EV_READY, 0, NULL, 0);
            } else {
                /* modem still booting: retry shortly */
                struct timespec ts = {0, 500 * 1000000L};
                nanosleep(&ts, NULL);
                pthread_mutex_lock(&r->mu);
                if (qmi_dev_is_up(r->dev)) r->need_init = 1;
                pthread_mutex_unlock(&r->mu);
            }
        } else if (have) {
            dispatch_event(r, &ev);
        }
        if (do_poll) {
            ts_after(&next_poll, 10000);
            if (r->ready) {
                ril_signal_t s;
                if (ril_net_signal(r, &s) == 0 && (!r->have_last_sig || sig_changed(&s, &r->last_sig))) {
                    r->last_sig = s;
                    r->have_last_sig = 1;
                    ril_ev_push(r, EV_SIG, 0, NULL, 0);
                }
            }
        }
    }
}

ril_qmi_t *ril_qmi_create(const char *dev_path, const ril_events_t *ev)
{
    struct ril_qmi *r = calloc(1, sizeof(*r));
    pthread_condattr_t ca;
    qmi_dev_cfg_t cfg;
    if (!r) return NULL;
    if (ev) r->ev = *ev;
    pthread_mutex_init(&r->mu, NULL);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&r->cv, &ca);
    pthread_condattr_destroy(&ca);

    memset(&cfg, 0, sizeof(cfg));
    cfg.path = dev_path;
    cfg.on_indication = on_indication;
    cfg.on_state = on_state;
    cfg.cb_ctx = r;
    cfg.auto_reconnect = 1;
    cfg.reconnect_min_ms = 500;
    cfg.reconnect_max_ms = 5000;
    /* The transport comes first: its initial on_state(1) only sets need_init and queues an event, the
     * worker (started afterwards) picks both up. This guarantees r->dev is valid before any use. */
    r->dev = qmi_dev_create(&cfg);
    if (!r->dev) {
        free(r);
        return NULL;
    }
    if (pthread_create(&r->worker, NULL, worker_main, r) != 0) {
        qmi_dev_destroy(r->dev);
        free(r);
        return NULL;
    }
    r->worker_started = 1;
    return r;
}

void ril_qmi_destroy(ril_qmi_t *r)
{
    if (!r) return;
    pthread_mutex_lock(&r->mu);
    r->stop = 1;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    if (r->dev) qmi_dev_destroy(r->dev); /* fails pending calls, stops the reader */
    if (r->worker_started) pthread_join(r->worker, NULL);
    pthread_cond_destroy(&r->cv);
    pthread_mutex_destroy(&r->mu);
    free(r);
}

int ril_qmi_ready(ril_qmi_t *r)
{
    int v;
    pthread_mutex_lock(&r->mu);
    v = r->ready;
    pthread_mutex_unlock(&r->mu);
    return v;
}

int ril_qmi_wait_ready(ril_qmi_t *r, int timeout_ms)
{
    struct timespec dl;
    ts_after(&dl, timeout_ms);
    pthread_mutex_lock(&r->mu);
    while (!r->ready) {
        if (pthread_cond_timedwait(&r->cv, &r->mu, &dl) == ETIMEDOUT) break;
    }
    timeout_ms = r->ready ? 0 : -ETIMEDOUT;
    pthread_mutex_unlock(&r->mu);
    return timeout_ms;
}
