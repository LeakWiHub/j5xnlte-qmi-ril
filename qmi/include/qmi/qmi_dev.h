/* SPDX-License-Identifier: Apache-2.0
 *
 * qmi_dev.h - thread-safe QMI transport over a QMUX character device
 *             (e.g. /dev/wwan0qmi0).
 *
 * Threading model
 *   * One reader thread per device performs poll()/read(), de-frames, routes
 *     responses to waiting callers and invokes the indication callback.
 *   * Any number of threads may call qmi_dev_call() concurrently; writes are
 *     serialised, every request gets a unique (service, client, txid) key.
 *   * Callbacks run on the reader thread: they MUST NOT call blocking
 *     qmi_dev_* functions (hand the work to another thread instead).
 *   * on_state() is delivered after the new state is already visible through
 *     qmi_dev_is_up()/qmi_dev_wait_up() (so a worker woken by the callback can
 *     immediately issue calls).  Do not assume it ran when wait_up() returns.
 *
 * Failure model
 *   * Read/write errors or POLLHUP mark the device down: every waiting call
 *     fails with -ENOTCONN, on_state(0) fires.  With auto_reconnect the reader
 *     keeps re-opening with exponential back-off; after a successful re-open
 *     on_state(1) fires.  ALL client IDs are invalid after a re-open, the
 *     upper layer must allocate them again.
 *   * A timed-out request may be re-sent with a NEW txid (retries argument).
 *     Only pass retries > 0 for idempotent requests (queries, not "send SMS").
 */
#ifndef QMI_DEV_H
#define QMI_DEV_H

#include <stddef.h>
#include <stdint.h>

#include "qmi/qmi_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qmi_dev qmi_dev_t;

typedef void (*qmi_ind_fn)(void *ctx, const qmi_msg_t *ind);
typedef void (*qmi_state_fn)(void *ctx, int up);
/* Returns an open, pollable fd or a negative errno. */
typedef int (*qmi_open_fn)(void *ctx);

typedef struct {
    const char *path;           /* default opener: open(path, O_RDWR|O_NONBLOCK) */
    qmi_open_fn open_fn;        /* optional override (used by the tests) */
    void *open_ctx;
    qmi_ind_fn on_indication;   /* optional */
    qmi_state_fn on_state;      /* optional */
    void *cb_ctx;
    int auto_reconnect;         /* keep re-opening after a failure */
    int reconnect_min_ms;       /* default 200 */
    int reconnect_max_ms;       /* default 5000 */
} qmi_dev_cfg_t;

typedef struct {
    unsigned long tx, rx_resp, rx_ind, stray_resp, timeouts, reconnects;
    unsigned long resyncs, dropped_bytes;
} qmi_dev_stats_t;

/* Error returns of qmi_dev_call_ok(): -(QMI_E_BASE + qmi_error_code). */
#define QMI_E_BASE 0x10000
#define QMI_E_IS_QMI(rc) ((rc) <= -QMI_E_BASE)
#define QMI_E_CODE(rc) ((uint16_t)(-(rc) - QMI_E_BASE))

/* Creates the device and starts the reader thread.  The first open is done
 * synchronously; if it fails and auto_reconnect is set the device is still
 * returned (down) and keeps retrying, otherwise NULL is returned with errno. */
qmi_dev_t *qmi_dev_create(const qmi_dev_cfg_t *cfg);
/* Stops the reader, fails pending calls, closes the fd.  Must not race with
 * calls that are still running on other threads. */
void qmi_dev_destroy(qmi_dev_t *d);

int qmi_dev_is_up(qmi_dev_t *d);
/* 0 when the device is up, -ETIMEDOUT otherwise. */
int qmi_dev_wait_up(qmi_dev_t *d, int timeout_ms);
void qmi_dev_get_stats(qmi_dev_t *d, qmi_dev_stats_t *out);

/* Synchronous request/response.
 *   rsp/rsp_cap receive the TLV region of the response (may be NULL/0 when
 *   the caller only cares about the result code); *rsp_len gets its size.
 * Returns 0 when a well formed response arrived (it may still carry a QMI
 * error: parse it, or use qmi_dev_call_ok), else a negative errno:
 *   -ETIMEDOUT, -ENOTCONN, -EIO (write failed), -EMSGSIZE, -EBUSY, -EPROTO. */
int qmi_dev_call(qmi_dev_t *d, uint8_t service, uint8_t client_id, uint16_t msg_id,
                 const uint8_t *tlvs, size_t tlv_len, int timeout_ms, int retries,
                 uint8_t *rsp, size_t rsp_cap, size_t *rsp_len);

/* Like qmi_dev_call but also folds the result TLV into the return value:
 * 0 = transport OK and QMI result SUCCESS; -(QMI_E_BASE+err) for a QMI error
 * response; other negative errno as above.  A response without a result TLV
 * is -EPROTO. */
int qmi_dev_call_ok(qmi_dev_t *d, uint8_t service, uint8_t client_id, uint16_t msg_id,
                    const uint8_t *tlvs, size_t tlv_len, int timeout_ms, int retries,
                    uint8_t *rsp, size_t rsp_cap, size_t *rsp_len);

/* CTL service helpers. */
int qmi_dev_alloc_client(qmi_dev_t *d, uint8_t service, uint8_t *client_id, int timeout_ms);
int qmi_dev_release_client(qmi_dev_t *d, uint8_t service, uint8_t client_id, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* QMI_DEV_H */
