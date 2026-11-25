/* SPDX-License-Identifier: Apache-2.0
 *
 * ril_qmi.h - synchronous "modem API" on top of qmi_dev: DMS / UIM / NAS / WMS / VOICE / WDS.
 *
 * All query functions are blocking (call them from binder threads, never from the callbacks) and
 * return 0 on success, or a negative value:  -ENOTCONN (modem/clients not ready), -ETIMEDOUT,
 * -ENOTSUP (service not provided by this modem), -(QMI_E_BASE + qmi_error) (QMI error, see qmi_dev.h),
 * other -errno.
 *
 * STATUS: message ids/TLVs marked [IDL] were cross-checked with libqril's IDL files, [LIBQMI] come from
 * public libqmi knowledge, [UNVERIFIED] were written from memory and have never talked to a real modem.
 */
#ifndef RIL_QMI_H
#define RIL_QMI_H

#include <stddef.h>
#include <stdint.h>

#include "qmi/qmi_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ril_qmi ril_qmi_t;

/* Callbacks run on the glue's own dispatcher thread (never on the QMI reader thread), so they may
 * call binder, but they should not block for long. */
typedef struct {
    void (*on_modem_state)(void *ctx, int ready);             /* clients allocated / lost */
    void (*on_sim_changed)(void *ctx);
    void (*on_network_changed)(void *ctx);
    void (*on_signal_changed)(void *ctx);
    void (*on_sms)(void *ctx, const uint8_t *pdu, size_t len, int ack_required); /* SMSC + TPDU */
    void (*on_calls_changed)(void *ctx);
    void (*on_data_status)(void *ctx, int connected);
    void *ctx;
    /* appended after ctx so older positional initialisers keep working (the field is then NULL).
     * type = android.hardware.radio.voice.UssdModeType: 0 NOTIFY, 1 REQUEST (answer needed), 2 NW_RELEASE */
    void (*on_ussd)(void *ctx, int type, const char *utf8);
} ril_events_t;

/* Starts the transport and the worker. Never blocks on the modem; ready state arrives via callback. */
ril_qmi_t *ril_qmi_create(const char *dev_path, const ril_events_t *ev);
void ril_qmi_destroy(ril_qmi_t *r);
int ril_qmi_ready(ril_qmi_t *r);
int ril_qmi_wait_ready(ril_qmi_t *r, int timeout_ms);

/* ---------------- DMS [IDL] ---------------- */
typedef struct {
    char imei[24], imeisv[8], esn[24], meid[24], revision[80];
} ril_ids_t;
int ril_modem_get_ids(ril_qmi_t *r, ril_ids_t *out);
#define RIL_OPMODE_ONLINE 0
#define RIL_OPMODE_LOW_POWER 1
int ril_modem_get_opmode(ril_qmi_t *r, int *mode);
int ril_modem_set_opmode(ril_qmi_t *r, int mode);

/* ---------------- UIM ---------------- */
typedef struct {
    int present;            /* card_state == present */
    int card_state;         /* 0 absent, 1 present, 2 error */
    int upin_state;
    int has_app;
    int app_type;           /* 1 SIM, 2 USIM, 3 RUIM, 4 CSIM, 5 ISIM */
    int app_state;          /* QmiUimCardApplicationState 0..7 */
    int perso_state;
    int upin_replaces_pin1;
    int pin1_state, pin1_retries, puk1_retries;
    int pin2_state, pin2_retries, puk2_retries;
    uint8_t aid[16];
    int aid_len;
    int provisioned;        /* primary GW provisioning session is bound to an application (index != 0xFFFF) */
    int slot;               /* 0-based slot of the reported application */
} ril_card_t;
int ril_sim_status(ril_qmi_t *r, ril_card_t *out);                 /* [IDL] 0x2F */
/* Binds the first SIM/USIM application of the present card to the primary GW provisioning session (UIM 0x38).
 * Without it the real modem keeps the application in state "detected" (index 0xFFFF) and never registers.
 * Returns 0 when already provisioned or after a successful request, -ENODEV when there is no usable application. */
int ril_sim_provision(ril_qmi_t *r);
int ril_sim_imsi(ril_qmi_t *r, char *out, size_t cap);              /* DMS 0x43 [IDL] */
int ril_sim_iccid(ril_qmi_t *r, char *out, size_t cap);             /* DMS 0x3C [IDL] */
#define RIL_PIN1 1
#define RIL_PIN2 2
/* PIN operations [LIBQMI]: 0x26 verify, 0x27 unblock, 0x28 change, 0x25 set protection.
 * retries_left receives the remaining verify attempts (or -1 when unknown). */
int ril_sim_verify_pin(ril_qmi_t *r, int pin_id, const char *pin, int *retries_left);
int ril_sim_unblock_pin(ril_qmi_t *r, int pin_id, const char *puk, const char *new_pin, int *retries_left);
int ril_sim_change_pin(ril_qmi_t *r, int pin_id, const char *old_pin, const char *new_pin, int *retries_left);
int ril_sim_set_pin_protection(ril_qmi_t *r, int pin_id, int enable, const char *pin, int *retries_left);
/* File access on the primary GSM/WCDMA provisioning session. path = sequence of 16-bit file ids in the
 * "3F00 7FFF" big-endian notation used by Android (converted to the little-endian QMI form inside). */
typedef struct {
    uint8_t sw1, sw2;
    uint8_t data[512];
    size_t len;
} ril_icc_result_t;
int ril_sim_read_transparent(ril_qmi_t *r, int file_id, const uint8_t *path, size_t path_len,
                             int offset, int length, ril_icc_result_t *out);
int ril_sim_read_record(ril_qmi_t *r, int file_id, const uint8_t *path, size_t path_len,
                        int record, int length, ril_icc_result_t *out);
/* out->data receives the raw FCP template (what a GET RESPONSE would return) */
int ril_sim_get_file_attrs(ril_qmi_t *r, int file_id, const uint8_t *path, size_t path_len, ril_icc_result_t *out);

/* ---------------- NAS [IDL] ---------------- */
#define RIL_RAT_NONE 0
#define RIL_RAT_GSM 4
#define RIL_RAT_UMTS 5
#define RIL_RAT_LTE 8
typedef struct {
    int reg_state;            /* 0 not reg, 1 registered, 2 searching, 3 denied, 4 unknown */
    int cs_attach, ps_attach; /* 0 unknown, 1 attached, 2 detached */
    int rat;                  /* RIL_RAT_* of the first reported interface */
    int roaming;              /* -1 unknown */
    int plmn_valid, mcc, mnc;
    int mnc_digits;           /* 2 or 3 when known, 0 = guess */
    char operator_name[66];
    int lac_valid, lac;
    int cid_valid;
    uint32_t cid;
} ril_serving_t;
int ril_net_serving(ril_qmi_t *r, ril_serving_t *out);
typedef struct {
    int gsm_valid, gsm_rssi;
    int wcdma_valid, wcdma_rssi, wcdma_ecio;
    int lte_valid, lte_rssi, lte_rsrq, lte_rsrp, lte_snr;
    int generic_valid, generic_dbm, generic_rat; /* from NAS 0x20 fallback */
} ril_signal_t;
int ril_net_signal(ril_qmi_t *r, ril_signal_t *out);
int ril_net_register_auto(ril_qmi_t *r);
int ril_net_register_manual(ril_qmi_t *r, int mcc, int mnc, int rat);
#define RIL_RATMASK_GSM 0x04   /* NAS mode preference bits [LIBQMI] */
#define RIL_RATMASK_UMTS 0x08
#define RIL_RATMASK_LTE 0x10
int ril_net_set_mode_pref(ril_qmi_t *r, unsigned mask);
int ril_net_get_mode_pref(ril_qmi_t *r, unsigned *mask);   /* RIL_RATMASK_* bits, [UNVERIFIED] TLV */
int ril_net_get_selection_mode(ril_qmi_t *r, int *manual);
typedef struct {
    int mcc, mnc, rat, status;  /* status bits per libqmi: [1:0] 1 current, 2 available; [5:4] 1 forbidden */
    char name[48];
} ril_scan_entry_t;
int ril_net_scan(ril_qmi_t *r, ril_scan_entry_t *out, size_t max, size_t *count); /* may take minutes */

/* ---------------- WMS [IDL] ---------------- */
/* pdu = SMSC address bytes followed by the TPDU (exactly what Android hands over) */
int ril_sms_send(ril_qmi_t *r, const uint8_t *pdu, size_t len, int *message_ref, int *error_class, int *cause);
int ril_sms_ack(ril_qmi_t *r, int success, int cause);   /* acknowledges the last received message */
int ril_sms_smsc(ril_qmi_t *r, char *out, size_t cap);

/* ---------------- VOICE [UNVERIFIED] ---------------- */
#define RIL_CALL_DIALING 2   /* values equal android.hardware.radio.voice.Call.STATE_* */
#define RIL_CALL_ACTIVE 0
#define RIL_CALL_HOLDING 1
#define RIL_CALL_ALERTING 3
#define RIL_CALL_INCOMING 4
#define RIL_CALL_WAITING 5
#define RIL_MAX_CALLS 8
typedef struct {
    int id;          /* modem call id */
    int state;       /* RIL_CALL_* */
    int incoming;    /* direction */
    int multiparty;
    char number[48];
} ril_call_t;
int ril_voice_dial(ril_qmi_t *r, const char *number, int *call_id);
int ril_voice_hangup(ril_qmi_t *r, int call_id);
int ril_voice_answer(ril_qmi_t *r, int call_id);
int ril_voice_calls(ril_qmi_t *r, ril_call_t *out, size_t max, size_t *count);   /* refreshes from the modem, falls back to the indication cache */
int ril_voice_dtmf(ril_qmi_t *r, int call_id, char digit, int start);

/* USSD [IDL per libqmi qmi-service-voice.json: Originate 0x3A, Answer 0x3B, Cancel 0x3C, indications 0x3D/0x3E].
 * Blocking (up to 100 s). Sends the text as a new request, or as the answer when the network is waiting for one.
 * reply gets the text carried by the response (may stay empty: then the text comes as an indication / on_ussd). */
int ril_ussd_send(ril_qmi_t *r, const char *utf8, char *reply, size_t cap);
int ril_ussd_cancel(ril_qmi_t *r);

/* ---------------- WDS ---------------- */
typedef struct {
    char apn[100], user[64], pass[64];
    int auth;       /* 0 none, 1 PAP, 2 CHAP, 3 both */
    int ip_family;  /* 4 or 6 (6 is requested but not configured by this glue) */
} ril_data_req_t;
typedef struct {
    uint32_t handle;
    char ifname[16];
    uint8_t ip[4], mask[4], gw[4], dns1[4], dns2[4];
    int has_ip, has_gw, has_dns1, has_dns2;
    int mtu;
} ril_data_call_t;
/* Starts a packet data call, brings the Linux interface up with the modem MTU and sets its IPv4 address (Android does
 * not for cellular). Routes and DNS are applied by the framework (netd) from the data call result. One call at a time. */
int ril_data_start(ril_qmi_t *r, const ril_data_req_t *req, ril_data_call_t *out);
int ril_data_stop(ril_qmi_t *r, uint32_t handle, const char *ifname);
/* interface to use: $RIL_DATA_IFACE override, then the first rmnet* (not rmnet_ipa*), then wwan* */
int ril_data_find_iface(char *out, size_t cap);
int ril_data_pkt_stats(ril_qmi_t *r, uint8_t *out, size_t cap, size_t *outlen); /* raw TLVs of WDS 0x24 */
void ril_data_diag(ril_qmi_t *r, const char *ifname, const char *tag);          /* kernel + modem counters into the log */

#ifdef __cplusplus
}
#endif
#endif /* RIL_QMI_H */
