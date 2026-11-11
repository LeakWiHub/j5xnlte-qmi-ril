#!/usr/bin/env python3
"""Replays responses RECORDED FROM A REAL j5xnlte modem (MPSS.DPM.2.0.2.c1.7, probe log of 2026-11-06, no SIM
registered, modem not online) to a program under test. Unlike fake_ril_modem.py this contains no modelling by me:
the response bytes are the real ones (IMEI replaced by a dummy; two probe runs; txid/cid are rewritten to match the request).
Requests that were not recorded get QMI error 0x0001 (MALFORMED_MSG) so that an unexpected request is visible.
usage: replay_real.py -- <command using {DEV}>
"""
import os, pty, subprocess, sys, termios, threading, time, tty
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_modem import tlv, frame, parse  # noqa: E402
import struct

H = lambda s: bytes.fromhex(s.replace(':', ''))

# (svc, request msg id) -> recorded response frame (service frames as seen on the wire)
REAL = {
    (2, 0x23): H('01:5B:00:80:02:01:02:01:00:23:00:4F:00:02:04:00:00:00:00:00:01:45:00:4D:50:53:53:2E:44:50:4D:2E:32:2E:30:2E:32:2E:63:31:2E:37:2D:30:30:30:36:37:2D:4D:38:39:33:36:46:41:41:41:41:4E:55:5A:4D:2D:31:20:20:31:20:20:5B:4D:61:79:20:31:34:20:32:30:31:39:20:30:31:3A:30:30:3A:30:30:5D'),
    # IMEI digits replaced by 350000000000008 (same length)
    (2, 0x25): H('01:45:00:80:02:01:02:02:00:25:00:39:00:02:04:00:00:00:00:00:13:01:00:31:12:0E:00:30:30:30:30:30:30:30:30:30:30:30:30:30:30:10:08:00:42:30:30:30:30:30:30:30:11:0F:00:33:35:30:30:30:30:30:30:30:30:30:30:30:30:38'),
    (2, 0x2D): H('01:17:00:80:02:01:02:03:00:2D:00:0B:00:02:04:00:00:00:00:00:01:01:00:05'),
    (0x0B, 0x2F): H('01:37:00:80:0B:01:02:01:00:2F:00:2B:00:02:04:00:00:00:00:00:15:03:00:02:00:00:11:03:00:02:00:00:10:15:00:FF:FF:FF:FF:FF:FF:FF:FF:02:02:00:00:00:03:00:02:00:00:00:03:00'),
    (3, 0x24): H('01:1B:00:80:03:01:02:01:00:24:00:0F:00:02:04:00:01:00:4A:00:01:05:00:00:00:00:00:00'),
    # --- second probe run (2026-11-09, after DMS set operating mode = online succeeded) ---
    (3, 0x4F): H('01:13:00:80:03:01:02:03:00:4F:00:07:00:02:04:00:01:00:4A:00'),   # no service: FAILURE / INFO_UNAVAILABLE
    (9, 0x2F): H('01:17:00:80:09:01:02:01:00:2F:00:0B:00:02:04:00:00:00:00:00:10:01:00:00'),  # no calls: call list is TLV 0x10, count 0
    (0x1A, 0x21): H('01:5D:00:80:1A:01:02:01:00:21:00:51:00:02:04:00:00:00:00:00:10:01:00:00:11:04:00:01:00:00:00:12:04:00:00:00:00:00:13:04:00:00:00:00:00:14:04:00:00:00:00:00:15:04:00:00:00:00:00:16:04:00:00:00:00:00:17:04:00:10:00:00:00:18:04:00:00:10:00:00:1A:04:00:00:00:00:00:19:04:00:01:00:00:00'),
    (3, 0x20): H('01:18:00:80:03:01:02:02:00:20:00:0C:00:02:04:00:00:00:00:00:01:02:00:80:00'),
}
# CTL Get Version Info: the capture in the log was cut off with "...", so the body is rebuilt from the printed service list
SERVICES = [(0, 1, 5), (1, 1, 36), (2, 1, 14), (3, 1, 25), (4, 1, 6), (5, 1, 10), (7, 1, 3), (8, 1, 2), (9, 2, 1),
            (0x0a, 2, 24), (0x0b, 1, 36), (0x0c, 1, 4), (0x0f, 1, 0), (0x10, 2, 0), (0x11, 1, 0), (0x17, 1, 0),
            (0x18, 1, 0), (0x1a, 1, 11), (0x1d, 1, 1), (0x22, 1, 0), (0x24, 1, 0), (0x29, 1, 0), (0x2a, 1, 0),
            (0x2b, 1, 0), (0x2e, 1, 0), (0x30, 1, 0), (0x36, 1, 0)]
REAL_BODY = {}   # custom answer bodies (captures that were cut off in the log)
# Scenario "online" (probe run 3, 2026-11-09): modem stayed online after the previous --online, no SIM, camping on UMTS 250/99 at -88 dBm
if os.environ.get('J5_REPLAY') == 'online':
    REAL[(2, 0x2D)] = H('01:17:00:80:02:01:02:03:00:2D:00:0B:00:02:04:00:00:00:00:00:01:01:00:00')
    REAL[(3, 0x24)] = H('01:4B:00:80:03:01:02:01:00:24:00:3F:00:02:04:00:00:00:00:00:01:06:00:02:02:02:02:01:05:10:01:00:00:11:01:00:00:12:05:00:FA:00:63:00:00:15:03:00:01:05:00:21:05:00:01:03:00:00:00:26:02:00:A9:01:27:05:00:FA:00:63:00:00:28:01:00:00')
    REAL[(3, 0x20)] = H('01:18:00:80:03:01:02:02:00:20:00:0C:00:02:04:00:00:00:00:00:01:02:00:A8:05')
    REAL[(3, 0x4F)] = H('01:1E:00:80:03:01:02:03:00:4F:00:12:00:02:04:00:00:00:00:00:13:03:00:A8:0A:00:20:02:00:04:00')
# Scenario "sim" (probe run 4, right after a reboot with a SIM inserted): slot 0 card present, USIM app only "detected", slot 1 error
if os.environ.get('J5_REPLAY') == 'sim':
    REAL[(0x0B, 0x2F)] = H('01:55:00:80:0B:01:02:01:00:2F:00:49:00:02:04:00:00:00:00:00:15:03:00:02:00:00:11:03:00:02:00:00:10:33:00:FF:FF:FF:FF:FF:FF:FF:FF:02:01:00:00:00:00:01:02:01:00:0B:00:00:10:A0:00:00:00:87:10:02:FF:47:F0:01:89:00:00:01:FF:00:00:00:00:00:00:00:02:00:00:00:03:00')
    # run 6 (--provision): the modem accepted UIM 0x38 and the next card status shows the USIM READY, PIN1 disabled, indices 0x0000
    REAL[(0x0B, 0x38)] = H('01:13:00:80:0B:01:02:02:00:38:00:07:00:02:04:00:00:00:00:00')
    AFTER_PROV = {(0x0B, 0x2F): H('01:55:00:80:0B:01:02:03:00:2F:00:49:00:02:04:00:00:00:00:00:15:03:00:02:00:00:11:03:00:02:00:00:10:33:00:00:00:FF:FF:FF:FF:FF:FF:02:01:00:00:00:00:01:02:07:02:0B:00:00:10:A0:00:00:00:87:10:02:FF:47:F0:01:89:00:00:01:FF:00:03:03:0A:01:03:0A:02:00:00:00:03:00')}
# Scenario "registered" (probe run 7): SIM READY, registered on LTE (250/20), -81 dBm. The 0x24 capture was cut after TLV 0x24, the TLVs before it are replayed.
if os.environ.get('J5_REPLAY') == 'registered':
    REAL[(2, 0x2D)] = H('01:17:00:80:02:01:02:03:00:2D:00:0B:00:02:04:00:00:00:00:00:01:01:00:00')   # online (after --online)
    REAL[(0x0B, 0x2F)] = H('01:55:00:80:0B:01:02:03:00:2F:00:49:00:02:04:00:00:00:00:00:15:03:00:02:00:00:11:03:00:02:00:00:10:33:00:00:00:FF:FF:FF:FF:FF:FF:02:01:00:00:00:00:01:02:07:02:0B:00:00:10:A0:00:00:00:87:10:02:FF:47:F0:01:89:00:00:01:FF:00:03:03:0A:01:03:0A:02:00:00:00:03:00')
    REAL[(3, 0x20)] = H('01:18:00:80:03:01:02:02:00:20:00:0C:00:02:04:00:00:00:00:00:01:02:00:AF:08')
    REAL[(3, 0x4F)] = H('01:21:00:80:03:01:02:03:00:4F:00:15:00:02:04:00:00:00:00:00:14:06:00:AF:F7:92:FF:16:00:20:02:00:01:00')
    REAL_BODY_SERVING = H('02:04:00:00:00:00:00:01:06:00:01:01:01:02:01:08:10:01:00:01:11:02:00:01:0B:12:0E:00:FA:00:14:00:09:D2:E7:94:5A:64:16:87:CF:26:15:03:00:01:08:01:1A:01:00:0C:1B:01:00:00:1C:02:00:FE:FF:1D:04:00:0C:3C:51:09:21:05:00:02:03:00:00:00:24:02:00:F7:66')
    REAL.pop((3, 0x24), None)
    REAL_BODY[(3, 0x24)] = REAL_BODY_SERVING
AFTER_PROV = globals().get('AFTER_PROV', {})
SYNC_IND = H('01:0B:00:80:00:00:02:00:27:00:00:00')   # recorded 8 times before the first answer
RES_OK = tlv(0x02, struct.pack('<HH', 0, 0))
# NAS Get System Selection Preference (0x34), real answer; the capture was cut after TLV 0x1C, so only the TLVs before the cut exist
REAL_BODY.update({(3, 0x34): RES_OK + H('10:01:00:00:11:02:00:1C:00:12:08:00:FF:FF:FF:BF:FF:FF:FF:FF:14:02:00:FF:00:15:08:00:FF:3F:DF:5B:FF:07:00:00:16:01:00:00:18:04:00:02:00:00:00:19:04:00:02:00:00:00:1A:08:00:3F:00:00:00:00:00:00:00:1C:04:00:03:08:05:04')})


class Replay:
    def __init__(self, master):
        self.m, self.missing, self.seen = master, [], []

    def reply(self, svc, cid, tx, msg, body):
        if svc == 0:
            if msg == 0x0021:
                b = RES_OK + tlv(0x01, bytes([len(SERVICES)]) + b''.join(struct.pack('<BHH', s, ma, mi) for s, ma, mi in SERVICES))
                return frame(0, 0, 'rsp', tx, msg, b)
            if msg == 0x0022:   # allocate CID: the real answers all had the shape {svc, cid=1}
                want = [v for t, v in self.tlvs(body) if t == 1][0][0]
                return frame(0, 0, 'rsp', tx, msg, RES_OK + tlv(0x01, bytes([want, 1])))
            if msg == 0x0023:
                return frame(0, 0, 'rsp', tx, msg, RES_OK + tlv(0x01, body[3:5] if len(body) >= 5 else b'\x00\x01'))
        if (svc, msg) in REAL_BODY:
            self.seen.append((svc, msg))
            return frame(svc, cid, 'rsp', tx, msg, REAL_BODY[(svc, msg)])
        if (svc, msg) == (0x0B, 0x38) and (svc, msg) in REAL:
            self.provisioned = True
        real = AFTER_PROV.get((svc, msg)) if getattr(self, 'provisioned', False) else None
        real = real or REAL.get((svc, msg))
        if real:
            r = parse(real)[1]
            self.seen.append((svc, msg))
            return frame(svc, cid, 'rsp', tx, msg, r[5])
        self.missing.append((svc, msg))
        return frame(svc, cid, 'rsp', tx, msg, tlv(0x02, struct.pack('<HH', 1, 1)))

    @staticmethod
    def tlvs(body):
        o = 0
        while o + 3 <= len(body):
            t, ln = struct.unpack_from('<BH', body, o)
            yield t, body[o + 3:o + 3 + ln]
            o += 3 + ln

    def serve(self):
        os.write(self.m, SYNC_IND * 8)   # the real modem queued these before the first request
        buf = b''
        while True:
            try:
                data = os.read(self.m, 4096)
            except OSError:
                return
            if not data:
                return
            buf += data
            while buf:
                r = parse(buf)
                if r is None:
                    break
                n, (flags, svc, cid, tx, msg, body) = r
                buf = buf[n:]
                os.write(self.m, self.reply(svc, cid, tx, msg, body))


def main():
    argv = sys.argv[1:]
    argv = argv[1:] if argv and argv[0] == '--' else argv
    master, slave = pty.openpty()
    tty.setraw(slave, termios.TCSANOW)
    dev = os.ttyname(slave)
    rp = Replay(master)
    threading.Thread(target=rp.serve, daemon=True).start()
    p = subprocess.run([a.replace('{DEV}', dev) for a in argv], capture_output=True, text=True, timeout=90)
    time.sleep(0.2)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    print('[replay] recorded responses served: %d, requests with no recording: %s' % (len(rp.seen), sorted(set(rp.missing))))
    if os.environ.get('J5_REPLAY') == 'sim':
        prov = getattr(rp, 'provisioned', False)
        print('[replay] UIM provisioning request (0x38) from the HAL core with the card "detected"/unprovisioned: %s' % ('SEEN' if prov else 'MISSING'))
        if not prov:
            return 1
    return p.returncode


if __name__ == '__main__':
    sys.exit(main())
