/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RIL_INTERNAL_H
#define RIL_INTERNAL_H

#include <errno.h>
#include <pthread.h>

#include "ril_qmi.h"

enum ril_ev_type { EV_READY = 1, EV_NOT_READY, EV_SIM, EV_NET, EV_SIG, EV_SMS, EV_CALLS, EV_DATA };

struct ril_ev_item {
    int type;
    int arg;
    size_t len;
    uint8_t data[300];
};

#define RIL_EVQ 64

struct ril_qmi {
    qmi_dev_t *dev;
    ril_events_t ev;

    pthread_mutex_t mu;
    pthread_cond_t cv; /* CLOCK_MONOTONIC */
    int ready;
    int stop;
    int need_init;
    pthread_t worker;
    int worker_started;

    uint8_t cid[0x30]; /* client id per QMI service id, 0 = not allocated */

    struct ril_ev_item q[RIL_EVQ];
    int qh, qn;
    unsigned long dropped_events;

    /* voice call cache, fed by All Call Status indications */
    ril_call_t calls[RIL_MAX_CALLS];
    size_t ncalls;

    /* SMS acknowledgement bookkeeping */
    uint32_t sms_txid;
    int sms_ack_pending;

    int prov_tries;     /* UIM provisioning attempts since the card last looked provisioned/absent */

    ril_signal_t last_sig;
    int have_last_sig;
};

/* Blocking request to a service using its allocated client id. Folds the QMI result TLV into the
 * return value (see qmi_dev_call_ok). rsp may be NULL when the body is not needed. */
int ril_svc(struct ril_qmi *r, uint8_t svc, uint16_t msg, const uint8_t *tlvs, size_t n, int timeout_ms,
            int retries, uint8_t *rsp, size_t cap, size_t *rlen);

/* Same, with the request built in a qmi_wbuf_t; refuses to send when the builder overflowed
 * (a truncated TLV set must never reach the modem). */
static inline int ril_svcw(struct ril_qmi *r, uint8_t svc, uint16_t msg, const qmi_wbuf_t *w, int timeout_ms, int retries,
                           uint8_t *rsp, size_t cap, size_t *rlen)
{
    if (!qmi_wbuf_ok(w)) return -EMSGSIZE;
    return ril_svc(r, svc, msg, w->buf, w->len, timeout_ms, retries, rsp, cap, rlen);
}

void ril_ev_push(struct ril_qmi *r, int type, int arg, const uint8_t *data, size_t len);

/* implemented in ril_svc_*.c */
void ril_nas_on_indication(struct ril_qmi *r, const qmi_msg_t *m);
void ril_uim_on_indication(struct ril_qmi *r, const qmi_msg_t *m);
void ril_wms_on_indication(struct ril_qmi *r, const qmi_msg_t *m);
void ril_voice_on_indication(struct ril_qmi *r, const qmi_msg_t *m);
void ril_wds_on_indication(struct ril_qmi *r, const qmi_msg_t *m);
void ril_nas_register(struct ril_qmi *r);
void ril_uim_register(struct ril_qmi *r);
void ril_wms_register(struct ril_qmi *r);
void ril_voice_register(struct ril_qmi *r);
void ril_wda_setup(struct ril_qmi *r);

#define RIL_SVC_TIMEOUT_MS 5000

#endif
