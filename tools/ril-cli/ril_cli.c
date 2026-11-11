/* SPDX-License-Identifier: Apache-2.0
 * ril-cli: drives the REAL ril core (ril/core + qmi/) from a shell, without Android and without the HAL.
 * This is the way to test SMS / calls / data / provisioning on a real modem before the HAL is built.
 * Everything it sends is what the HAL would send. It can cost money (SMS, calls, data): use a number/APN you mean.
 *
 *   ril-cli [-d /dev/wwan0qmi0] status
 *   ril-cli online                            set the modem online (the HAL does this from setRadioPower)
 *   ril-cli listen [seconds]                  print network/signal/SIM/call/SMS events (acks incoming SMS)
 *   ril-cli sms-send <number> <text>          GSM-7 text, ASCII letters/digits/basic punctuation only
 *   ril-cli dial <number> [hold_seconds]      dial, print call states, hang up after hold_seconds (default 15)
 *   ril-cli answer [wait_seconds]             wait for an incoming call and answer it, hold 15 s, hang up
 *   ril-cli data <apn> [hold_seconds]         start a data call, print the settings, stop after hold_seconds
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ril_qmi.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int ev_sim, ev_net, ev_sig, ev_calls, ev_data = -1, ev_ready = -1;
static uint8_t sms_buf[512];
static size_t sms_len;
static int sms_ack_req, sms_pending;

static void on_state(void *c, int ready) { (void)c; pthread_mutex_lock(&mu); ev_ready = ready; pthread_mutex_unlock(&mu); }
static void on_sim(void *c) { (void)c; pthread_mutex_lock(&mu); ev_sim++; pthread_mutex_unlock(&mu); }
static void on_net(void *c) { (void)c; pthread_mutex_lock(&mu); ev_net++; pthread_mutex_unlock(&mu); }
static void on_sig(void *c) { (void)c; pthread_mutex_lock(&mu); ev_sig++; pthread_mutex_unlock(&mu); }
static void on_calls(void *c) { (void)c; pthread_mutex_lock(&mu); ev_calls++; pthread_mutex_unlock(&mu); }
static void on_data(void *c, int up) { (void)c; pthread_mutex_lock(&mu); ev_data = up; pthread_mutex_unlock(&mu); }
static void on_sms(void *c, const uint8_t *pdu, size_t len, int ack)
{
    (void)c;
    pthread_mutex_lock(&mu);
    sms_len = len > sizeof(sms_buf) ? sizeof(sms_buf) : len;
    memcpy(sms_buf, pdu, sms_len);
    sms_ack_req = ack;
    sms_pending = 1;
    pthread_mutex_unlock(&mu);
}

static void hex(const uint8_t *p, size_t n) { size_t i; for (i = 0; i < n; i++) printf("%02X", p[i]); }
static void msleep(int ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
static long now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }

/* ---- SMS helpers: GSM 7-bit default alphabet restricted to the part that equals ASCII ---- */
static int gsm7_ok(char c)
{
    if (c < 0x20 || c > 0x7E) return 0;
    return !strchr("@$_[]\\^{}|~`", c);
}

static size_t pack7(const char *t, size_t n, uint8_t *out)
{
    size_t i, o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (i = 0; i < n; i++) {
        acc |= ((unsigned)(uint8_t)t[i] & 0x7F) << bits;
        bits += 7;
        while (bits >= 8) { out[o++] = (uint8_t)acc; acc >>= 8; bits -= 8; }
    }
    if (bits) out[o++] = (uint8_t)acc;
    return o;
}

static size_t build_submit(const char *num, const char *text, uint8_t *pdu)
{
    size_t o = 0, nd = 0, i, tl = strlen(text);
    int intl = num[0] == '+';
    const char *d = intl ? num + 1 : num;
    uint8_t bcd[16];
    if (tl == 0 || tl > 160) return 0;
    for (i = 0; i < tl; i++) if (!gsm7_ok(text[i])) return 0;
    for (i = 0; d[i]; i++) { if (d[i] < '0' || d[i] > '9' || nd >= 20) return 0; nd++; }
    if (nd < 3) return 0;
    pdu[o++] = 0x00;           /* SMSC: length 0 = use the modem/SIM default */
    pdu[o++] = 0x01;           /* SMS-SUBMIT, relative validity absent */
    pdu[o++] = 0x00;           /* message reference: the modem assigns it */
    pdu[o++] = (uint8_t)nd;
    pdu[o++] = intl ? 0x91 : 0x81;
    memset(bcd, 0xFF, sizeof(bcd));
    for (i = 0; i < nd; i++) {
        uint8_t v = (uint8_t)(d[i] - '0');
        if (i % 2 == 0) bcd[i / 2] = (uint8_t)(0xF0 | v); else bcd[i / 2] = (uint8_t)((bcd[i / 2] & 0x0F) | (v << 4));
    }
    memcpy(pdu + o, bcd, (nd + 1) / 2);
    o += (nd + 1) / 2;
    pdu[o++] = 0x00;           /* PID */
    pdu[o++] = 0x00;           /* DCS: GSM 7-bit */
    pdu[o++] = (uint8_t)tl;
    o += pack7(text, tl, pdu + o);
    return o;
}

static void unpack7(const uint8_t *p, size_t septets, char *out)
{
    size_t i;
    for (i = 0; i < septets; i++) {
        size_t bit = i * 7, b = bit / 8, s = bit % 8;
        unsigned v = p[b] >> s;
        if (s > 1) v |= (unsigned)p[b + 1] << (8 - s);
        v &= 0x7F;
        out[i] = (v >= 0x20 && v < 0x7F) ? (char)v : '?';
    }
    out[septets] = 0;
}

static void show_sms(const uint8_t *pdu, size_t n)
{
    size_t o = 0, i;
    char from[24] = "?", text[200] = "";
    printf("  SMS PDU: "); hex(pdu, n); printf("\n");
    if (n < 2) return;
    o = 1u + pdu[0];                       /* skip SMSC */
    if (o + 12 > n || (pdu[o] & 3) != 0) { printf("  (not an SMS-DELIVER I can decode)\n"); return; }
    {
        size_t nd = pdu[o + 1], k = 0;
        int intl = (pdu[o + 2] & 0x70) == 0x10;
        const uint8_t *a = pdu + o + 3;
        if (intl) from[k++] = '+';
        for (i = 0; i < nd && k < 22; i++) { uint8_t v = (i % 2) ? (uint8_t)(a[i / 2] >> 4) : (uint8_t)(a[i / 2] & 0x0F); from[k++] = (char)('0' + v); }
        from[k] = 0;
        o += 3 + (nd + 1) / 2;                 /* PID, DCS, timestamp follow */
        if (o + 9 <= n && pdu[o + 1] == 0x00) { /* DCS 0 = GSM 7-bit */
            size_t udl = pdu[o + 8];
            if (udl < 160 && o + 9 + (udl * 7 + 7) / 8 <= n) unpack7(pdu + o + 9, udl, text);
        }
    }
    printf("  from %s: %s\n", from, text);
}

static int prefix_len(const uint8_t *m)
{
    int n = 0, i, b;
    for (i = 0; i < 4; i++) for (b = 7; b >= 0; b--) if (m[i] & (1 << b)) n++; else return n;
    return n;
}

static void print_status(ril_qmi_t *r)
{
    ril_ids_t ids; ril_card_t c; ril_serving_t sv; ril_signal_t sg; char s[40]; int rc, mode = -1; unsigned mask = 0;
    rc = ril_modem_get_ids(r, &ids);
    printf("modem ids:  rc=%d imei=%s revision=%s\n", rc, rc ? "?" : ids.imei, rc ? "?" : ids.revision);
    rc = ril_modem_get_opmode(r, &mode); printf("op mode:    rc=%d mode=%d (0 = online)\n", rc, mode);
    rc = ril_sim_status(r, &c);
    printf("sim:        rc=%d present=%d app_type=%d app_state=%d (7 = ready) provisioned=%d pin1_state=%d retries=%d/%d\n", rc, c.present, c.app_type, c.app_state, c.provisioned, c.pin1_state, c.pin1_retries, c.puk1_retries);
    rc = ril_sim_imsi(r, s, sizeof(s)); printf("imsi:       rc=%d %s\n", rc, rc ? "" : s);
    rc = ril_sim_iccid(r, s, sizeof(s)); printf("iccid:      rc=%d %s\n", rc, rc ? "" : s);
    rc = ril_net_serving(r, &sv);
    printf("network:    rc=%d reg=%d (1 home, 5 roaming, 2 searching, 3 denied) cs=%d ps=%d rat=%d plmn=%d/%d name='%s' roaming=%d\n", rc, sv.reg_state, sv.cs_attach, sv.ps_attach, sv.rat, sv.mcc, sv.mnc, sv.operator_name, sv.roaming);
    rc = ril_net_signal(r, &sg);
    printf("signal:     rc=%d lte=%d(rssi %d rsrp %d rsrq %d snr %d) wcdma=%d(rssi %d) gsm=%d(rssi %d)\n", rc, sg.lte_valid, sg.lte_rssi, sg.lte_rsrp, sg.lte_rsrq, sg.lte_snr, sg.wcdma_valid, sg.wcdma_rssi, sg.gsm_valid, sg.gsm_rssi);
    rc = ril_net_get_mode_pref(r, &mask); printf("mode pref:  rc=%d mask=0x%x\n", rc, mask);
    rc = ril_sim_status(r, &c); (void)rc;
    rc = ril_sms_smsc(r, s, sizeof(s)); printf("smsc:       rc=%d %s\n", rc, rc ? "" : s);
}

static void show_calls(ril_qmi_t *r, int *last_sig)
{
    ril_call_t cl[RIL_MAX_CALLS]; size_t n = 0, i; int rc = ril_voice_calls(r, cl, RIL_MAX_CALLS, &n);
    int sig = rc ? -1 : (int)n * 100;
    for (i = 0; i < n; i++) sig += cl[i].state * (int)(i + 1) * 7 + cl[i].id;
    if (sig == *last_sig) return;
    *last_sig = sig;
    printf("  calls: rc=%d count=%zu\n", rc, n);
    for (i = 0; i < n; i++) printf("    id=%d state=%d (0 active,1 hold,2 dialing,3 alerting,4 incoming,5 waiting) incoming=%d number='%s'\n", cl[i].id, cl[i].state, cl[i].incoming, cl[i].number);
}


static void print_pkt_stats(ril_qmi_t *r, const char *tag)
{
    uint8_t buf[256]; size_t n = 0, i; int rc = ril_data_pkt_stats(r, buf, sizeof(buf), &n);
    printf("  %s: rc=%d", tag, rc);
    if (!rc) {   /* raw TLVs: type, len, value (little endian) */
        size_t off = 0;
        while (off + 3 <= n) {
            unsigned t = buf[off], l = buf[off + 1] | (buf[off + 2] << 8);
            if (off + 3 + l > n) break;
            printf(" [%02x:", t);
            for (i = 0; i < l; i++) printf("%02x", buf[off + 3 + i]);
            printf("]");
            off += 3 + l;
        }
    }
    printf("\n");
}

static int safe_name(const char *s) { for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return 0; return 1; }

/* RIL_SELFTEST=1: apply the address/route and ping through the data interface, printing the kernel ICMP counters around it */
static void selftest(const ril_data_call_t *dc)
{
    char cmd[512];
    const char *icmp = "awk '/^Icmp:/{if(!h){split($0,k);h=1}else{split($0,v);for(i=2;i<=NF;i++)if(k[i]~/^(OutMsgs|OutEchos|InMsgs|InEchoReps)$/)printf \"%s=%s \",k[i],v[i];print \"\"}}' /proc/net/snmp";
    if (!safe_name(dc->ifname)) return;
    /* the BAM-DMUX channel is opened towards the modem by ndo_open (OPEN command): redo it now that the call is bound */
    if (getenv("RIL_SELFTEST_RELINK")) {
        snprintf(cmd, sizeof(cmd), "ip link set %s down; ip link set %s up", dc->ifname, dc->ifname);
        printf("  selftest: %s\n", cmd); fflush(stdout); (void)system(cmd);
    }
    snprintf(cmd, sizeof(cmd), "ip addr add %u.%u.%u.%u/%d dev %s 2>&1; ip route add 8.8.8.8 dev %s 2>&1", dc->ip[0], dc->ip[1], dc->ip[2], dc->ip[3], prefix_len(dc->mask), dc->ifname, dc->ifname);
    printf("  selftest: %s\n", cmd); fflush(stdout); (void)system(cmd);
    printf("  icmp before: "); fflush(stdout); (void)system(icmp);
    snprintf(cmd, sizeof(cmd), "ping -c 3 -W 2 -I %s 8.8.8.8 2>&1 | tail -3", dc->ifname);
    fflush(stdout); (void)system(cmd);
    printf("  icmp after: "); fflush(stdout); (void)system(icmp);
    (void)system("echo '  dmesg bam/dmux:'; dmesg | grep -iE 'bam|dmux' | tail -15");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0); /* output must appear at once when redirected to a file */
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *dev = "/dev/wwan0qmi0";
    const char *cmd;
    ril_events_t ev = {on_state, on_sim, on_net, on_sig, on_sms, on_calls, on_data, NULL};
    ril_qmi_t *r;
    int a = 1, rc, last_calls = -2;
    long end;
    if (argc > 2 && !strcmp(argv[1], "-d")) { dev = argv[2]; a = 3; }
    if (a >= argc) { fprintf(stderr, "usage: see the header of tools/ril-cli/ril_cli.c\n"); return 2; }
    cmd = argv[a];
    r = ril_qmi_create(dev, &ev);
    if (!r) { fprintf(stderr, "cannot start (device %s)\n", dev); return 1; }
    if (ril_qmi_wait_ready(r, 15000) != 0) { fprintf(stderr, "modem clients not ready within 15 s\n"); ril_qmi_destroy(r); return 1; }
    /* the core provisions the SIM application by itself; give it a moment so that `status` shows the result */
    msleep(1500);

    if (!strcmp(cmd, "online")) {
        int mode = -1;
        rc = ril_modem_set_opmode(r, RIL_OPMODE_ONLINE);
        printf("set online: rc=%d\n", rc);
        for (a = 0; a < 10; a++) { msleep(1000); if (ril_modem_get_opmode(r, &mode) == 0 && mode == 0) break; }
        printf("op mode now: %d (0 = online)\n", mode);
    } else if (!strcmp(cmd, "status")) {
        print_status(r);
    } else if (!strcmp(cmd, "listen") || !strcmp(cmd, "answer")) {
        int answering = !strcmp(cmd, "answer"), answered = 0, secs = a + 1 < argc ? atoi(argv[a + 1]) : (answering ? 60 : 120);
        long answered_at = 0;
        end = now_s() + secs;
        printf("%s for %d s ... (call or text this phone now)\n", answering ? "waiting for an incoming call" : "listening", secs);
        printf("\n");
        while (now_s() < end) {
            int s, n, sg, c, p, ack;
            uint8_t pdu[512]; size_t pl;
            pthread_mutex_lock(&mu);
            s = ev_sim; ev_sim = 0; n = ev_net; ev_net = 0; sg = ev_sig; ev_sig = 0; c = ev_calls; ev_calls = 0;
            p = sms_pending; sms_pending = 0; pl = sms_len; ack = sms_ack_req; memcpy(pdu, sms_buf, pl);
            pthread_mutex_unlock(&mu);
            if (s) printf("[event] SIM changed\n");
            if (n) { ril_serving_t sv; rc = ril_net_serving(r, &sv); printf("[event] network changed: rc=%d reg=%d rat=%d plmn=%d/%d\n", rc, sv.reg_state, sv.rat, sv.mcc, sv.mnc); }
            if (sg) { ril_signal_t g; rc = ril_net_signal(r, &g); printf("[event] signal: rc=%d lte=%d rsrp=%d wcdma=%d rssi=%d\n", rc, g.lte_valid, g.lte_rsrp, g.wcdma_valid, g.wcdma_rssi); }
            if (c) { printf("[event] calls changed\n"); last_calls = -2; }
            if (p) { printf("[event] SMS received (ack required: %d)\n", ack); show_sms(pdu, pl); if (ack) { rc = ril_sms_ack(r, 1, 0); printf("  ack: rc=%d\n", rc); } }
            if (c || answering) show_calls(r, &last_calls);
            if (answering) {
                ril_call_t cl[RIL_MAX_CALLS]; size_t cn = 0, i;
                if (!answered && ril_voice_calls(r, cl, RIL_MAX_CALLS, &cn) == 0)
                    for (i = 0; i < cn; i++) if (cl[i].state == RIL_CALL_INCOMING) { rc = ril_voice_answer(r, cl[i].id); printf("answer call %d: rc=%d\n", cl[i].id, rc); answered = 1; answered_at = now_s(); }
                if (answered && now_s() - answered_at >= 15) {
                    if (ril_voice_calls(r, cl, RIL_MAX_CALLS, &cn) == 0) for (i = 0; i < cn; i++) printf("hangup %d: rc=%d\n", cl[i].id, ril_voice_hangup(r, cl[i].id));
                    break;
                }
            }
            msleep(300);
        }
    } else if (!strcmp(cmd, "sms-send") && a + 2 < argc) {
        uint8_t pdu[200]; size_t n = build_submit(argv[a + 1], argv[a + 2], pdu); int ref = -1, cls = -1, cause = -1;
        if (!n) { fprintf(stderr, "cannot encode: number needs 3-20 digits (optional +), text 1-160 chars of ASCII without @ $ _ [ ] \\ ^ { } | ~ `\n"); ril_qmi_destroy(r); return 2; }
        printf("sending %zu bytes: ", n); hex(pdu, n); printf("\n");
        rc = ril_sms_send(r, pdu, n, &ref, &cls, &cause);
        printf("send result: rc=%d message_ref=%d error_class=%d cause=%d  (rc=0 = the network accepted it)\n", rc, ref, cls, cause);
    } else if (!strcmp(cmd, "dial") && a + 1 < argc) {
        int hold = a + 2 < argc ? atoi(argv[a + 2]) : 15, id = -1; long t0;
        rc = ril_voice_dial(r, argv[a + 1], &id);
        printf("dial %s: rc=%d call_id=%d\n", argv[a + 1], rc, id);
        t0 = now_s();
        while (!rc && now_s() - t0 < hold) { show_calls(r, &last_calls); msleep(500); }
        if (!rc) { rc = ril_voice_hangup(r, id); printf("hangup: rc=%d\n", rc); msleep(1500); show_calls(r, &last_calls); }
    } else if (!strcmp(cmd, "data") && a + 1 < argc) {
        ril_data_req_t rq; ril_data_call_t dc; int hold = a + 2 < argc ? atoi(argv[a + 2]) : 20;
        memset(&rq, 0, sizeof(rq)); memset(&dc, 0, sizeof(dc));
        snprintf(rq.apn, sizeof(rq.apn), "%s", argv[a + 1]); rq.ip_family = 4;
        rc = ril_data_start(r, &rq, &dc);
        printf("data start: rc=%d handle=%u iface=%s mtu=%d\n", rc, dc.handle, dc.ifname, dc.mtu);
        if (!rc) {
            printf("  ip %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u dns %u.%u.%u.%u %u.%u.%u.%u (has ip/gw/dns1/dns2: %d/%d/%d/%d)\n", dc.ip[0], dc.ip[1], dc.ip[2], dc.ip[3], dc.mask[0], dc.mask[1], dc.mask[2], dc.mask[3], dc.gw[0], dc.gw[1], dc.gw[2], dc.gw[3], dc.dns1[0], dc.dns1[1], dc.dns1[2], dc.dns1[3], dc.dns2[0], dc.dns2[1], dc.dns2[2], dc.dns2[3], dc.has_ip, dc.has_gw, dc.has_dns1, dc.has_dns2);
            printf("  to test traffic, in ANOTHER shell now (root):\n");
            printf("    ip addr add %u.%u.%u.%u/%d dev %s\n    ip route add 8.8.8.8 dev %s\n    ping -c 3 -I %s 8.8.8.8\n", dc.ip[0], dc.ip[1], dc.ip[2], dc.ip[3], prefix_len(dc.mask), dc.ifname, dc.ifname, dc.ifname);
            print_pkt_stats(r, "modem counters at start");
            if (getenv("RIL_SELFTEST")) selftest(&dc);
            printf("  holding the call for %d s ...\n", hold);
            msleep(hold * 1000);
            print_pkt_stats(r, "modem counters before stop");
            rc = ril_data_stop(r, dc.handle, dc.ifname);
            printf("data stop: rc=%d\n", rc);
        }
    } else {
        fprintf(stderr, "unknown or incomplete command: %s\n", cmd);
        ril_qmi_destroy(r);
        return 2;
    }
    ril_qmi_destroy(r);
    return 0;
}
