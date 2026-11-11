#!/usr/bin/env python3
"""Stateful fake QMI modem for the RIL glue test (tests/test_ril_glue.c).

Written separately from the C code from the libqril IDL files (qril_*.qmi) and public QMI conventions.
It proves that the glue encodes/decodes consistently with those documents. It does NOT prove anything about
the real MSM8916 modem; the VOICE service has no IDL reference at all and is modelled from memory.

usage: fake_ril_modem.py -- <command using {DEV}> ...
exit code is non-zero when the command failed OR a modem-side check failed.
"""
import os, pty, struct, subprocess, sys, termios, threading, time, tty
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fake_modem import tlv, OK, frame, parse, card_status  # noqa: E402

SEND_PDU = bytes.fromhex('00110007916407281553F80000AA0AE8329BFD4697D9EC37')
SEND_PDU_FAIL = SEND_PDU[:-1] + b'\xFF'
MT_SMS = bytes.fromhex('0791448720003023040B916407281553F80000FF0AE8329BFD4697D9EC37')
MT_TXID = 0x1234
ERR = lambda code: tlv(0x02, struct.pack('<HH', 1, code))
ip_wire = lambda a: bytes(reversed(bytes(a)))


class Modem:
    def __init__(self, master):
        self.m = master
        self.lock = threading.Lock()
        self.cids = {}
        self.itx = 100
        self.calls = {}        # id -> [state, dir, number]
        self.checks = []       # (name, ok)
        self.seen = set()
        self.data_handle = 0xCAFE0001
        self.data_up = False
        self.unprov = os.environ.get('FAKE_UIM_UNPROVISIONED') == '1'   # real behaviour: app 'detected' until UIM 0x38
        self.prov_requests = 0

    def check(self, name, ok):
        self.checks.append((name, bool(ok)))

    def send(self, svc, kind, tx, msg, body):
        with self.lock:
            os.write(self.m, frame(svc, self.cids.get(svc, 0), kind, tx, msg, body))

    def ind(self, svc, msg, body):
        self.itx += 1
        self.send(svc, 'ind', self.itx, msg, body)

    def later(self, delay, fn, *a):
        t = threading.Timer(delay, fn, a)
        t.daemon = True
        t.start()

    # ---- voice helpers
    def call_tlvs(self, tc=0x01, tn=0x10):
        items = [(i, c) for i, c in sorted(self.calls.items())]
        b = tlv(tc, bytes([len(items)]) + b''.join(bytes([i, c[0], 0, c[1], 1, 0, 0]) for i, c in items))
        nums = [(i, c[2]) for i, c in items if c[2]]
        if nums:
            b += tlv(tn, bytes([len(nums)]) + b''.join(bytes([i, 0, len(n)]) + n.encode() for i, n in nums))
        return b

    def push_calls(self):
        self.ind(9, 0x2E, self.call_tlvs())

    def set_state(self, cid, st):
        if cid in self.calls:
            self.calls[cid][0] = st
            self.push_calls()

    # ---- request handling
    def handle(self, svc, cid, tx, msg, body):
        from_tlv = lambda t: dict((a, b) for a, b in self.tlvs(body)).get(t)
        if svc == 0:
            if msg == 0x0021:
                ents = [(1, 1, 0), (2, 1, 0), (3, 1, 0), (5, 1, 0), (9, 1, 0), (11, 1, 0), (26, 1, 0)]
                return OK + tlv(0x01, bytes([len(ents)]) + b''.join(struct.pack('<BHH', *e) for e in ents))
            if msg == 0x0022:
                s = body[3]
                self.cids[s] = 0x10 + s
                return OK + tlv(0x01, bytes([s, self.cids[s]]))
            if msg == 0x0023:
                return OK + tlv(0x01, body[3:5])
        if svc == 2:   # DMS
            if msg == 0x0023: return OK + tlv(0x01, b'MPSS.TH.2.0-FAKE')
            if msg == 0x0025: return OK + tlv(0x11, b'351234567890123') + tlv(0x13, b'01')
            if msg == 0x0043: return OK + tlv(0x01, b'250011234567890')
            if msg == 0x003C: return OK + tlv(0x01, b'8970101234567890123')
            if msg == 0x002D: return OK + tlv(0x01, b'\x00') + tlv(0x11, b'\x00')
            if msg == 0x002E:
                self.check('dms set operating mode carries a mode byte', from_tlv(0x01) in (b'\x00', b'\x01'))
                return OK
        if svc == 11:  # UIM
            if msg == 0x002E:
                self.later(0.3, self.ind, 11, 0x0032, tlv(0x10, b'\x00'))
                return OK
            if msg == 0x0038:
                self.prov_requests += 1
                self.check('uim provisioning: session {primary GW, activate}', from_tlv(0x01) == b'\x00\x01')
                self.check('uim provisioning: slot 1 + the 16-byte AID of the app', from_tlv(0x10) == b'\x01\x10' + bytes.fromhex('a0000000871002ff47f001890000 01ff'.replace(' ', '')))
                self.unprov = False
                self.later(0.2, self.ind, 11, 0x0032, tlv(0x10, b'\x00'))
                return OK
            if msg == 0x002F: return OK + tlv(0x10, card_status(True, provisioned=not self.unprov))
            if msg in (0x0020, 0x0021, 0x0024):
                sess, fil = from_tlv(0x01), from_tlv(0x02)
                self.check('uim session TLV = type 0 + empty aid', sess == b'\x00\x00')
                fid = struct.unpack_from('<H', fil, 0)[0]
                plen = fil[2]
                self.check('uim path is little endian ids (00 3F FF 7F)', fil[3:3 + plen] == bytes.fromhex('003FFF7F'))
                if msg == 0x0024:
                    raw = bytes.fromhex('621E8202412183027FFF8A01058B032F0603800200098800')
                    hdr = struct.pack('<HHBHH', 9, fid, 0, 0, 0) + bytes([0, 0, 0] * 5)
                    hdr = struct.pack('<HHBHH', 9, fid, 0, 0, 0) + b''.join(struct.pack('<BH', 0, 0) for _ in range(5))
                    return OK + tlv(0x10, b'\x90\x00') + tlv(0x11, hdr + struct.pack('<H', len(raw)) + raw)
                ri = from_tlv(0x03)
                off, ln = struct.unpack('<HH', ri)
                self.check('uim read_info decoded (offset 0, len 9)', (off, ln) == (0, 9))
                data = bytes.fromhex('08 29 05 10 21 43 65 87 F9'.replace(' ', ''))
                return OK + tlv(0x10, b'\x90\x00') + tlv(0x11, struct.pack('<H', len(data)) + data)
            if msg == 0x0026:
                p = from_tlv(0x02)
                pin = p[2:2 + p[1]]
                if pin == b'1234': return OK
                return ERR(0x000C) + tlv(0x10, bytes([2, 10]))
        if svc == 3:   # NAS
            if msg == 0x0003:
                self.later(0.3, self.ind, 3, 0x0024, tlv(0x01, bytes([1, 1, 1, 2, 1, 8])))
                self.later(0.4, self.ind, 3, 0x0051, tlv(0x14, struct.pack('<bbhh', -60, -9, -95, 120)))
                return OK
            if msg == 0x0024:
                if os.environ.get('FAKE_NAS_NOSERVICE_FIRST') and not getattr(self, 'ns_done', False):
                    # frame layout captured from the real modem (MPSS.DPM.2.0.2, no service): FAILURE/INFO_UNAVAILABLE
                    # with TLV 0x01 = 00 00 00 00 00 ("not registered")
                    self.ns_done = True
                    return ERR(0x004A) + tlv(0x01, bytes(5))
                desc = b'MTS'
                return (OK + tlv(0x01, bytes([1, 1, 1, 2, 1, 8])) + tlv(0x12, struct.pack('<HHB', 250, 1, len(desc)) + desc) +
                        tlv(0x1D, struct.pack('<H', 0x1234)) + tlv(0x1E, struct.pack('<H', 0xBEEF)))
            if msg == 0x004F: return OK + tlv(0x14, struct.pack('<bbhh', -60, -9, -95, 120))
            if msg == 0x0022:
                self.check('nas register auto', from_tlv(0x01) == b'\x01')
                return OK
            if msg == 0x0034: return OK + tlv(0x16, b'\x00')
            if msg == 0x0033: return OK
        if svc == 5:   # WMS
            if msg == 0x0032:
                r = from_tlv(0x01)
                n = struct.unpack_from('<H', r, 0)[0]
                ents = [r[2 + 4 * i: 6 + 4 * i] for i in range(n)]
                self.check('wms routes: 5 x (p2p, class i, storage none, transfer only)',
                           n == 5 and all(e == bytes([0, i, 0xFF, 2]) for i, e in enumerate(ents)))
                return OK
            if msg == 0x0001:
                self.check('wms new MT indicator', from_tlv(0x10) == b'\x01')
                body = struct.pack('<BIBH', 0, MT_TXID, 6, len(MT_SMS)) + MT_SMS
                self.later(0.5, self.ind, 5, 0x0001, tlv(0x11, body))
                return OK
            if msg == 0x0020:
                m = from_tlv(0x01)
                fmt, ln = struct.unpack_from('<BH', m, 0)
                data = m[3:3 + ln]
                self.check('wms raw send format 3GPP + length', fmt == 6 and len(m) == 3 + ln)
                if data == SEND_PDU:
                    self.check('wms raw send pdu intact', True)
                    return OK + tlv(0x01, struct.pack('<H', 7))
                self.check('wms raw send (failure case) pdu intact', data == SEND_PDU_FAIL)
                return ERR(0x0004) + tlv(0x10, struct.pack('<H', 0x0123)) + tlv(0x11, b'\x02')
            if msg == 0x0037:
                a = from_tlv(0x01)
                txid, proto, ok = struct.unpack('<IBB', a)
                self.check('wms ack carries the MT transaction id, 3GPP, success', (txid, proto, ok) == (MT_TXID, 1, 1))
                self.seen.add('ack')
                return OK
            if msg == 0x0034: return OK + tlv(0x01, b'145' + bytes([11]) + b'79037029984')
        if svc == 9:   # VOICE (modelled from memory, no IDL available)
            if msg == 0x0003: return OK
            if msg == 0x002F: return OK + self.call_tlvs(0x10, 0x11)   # layout of the real answer (calls in 0x10)
            if msg == 0x0020:
                num = from_tlv(0x01)
                self.check('voice dial number', num == b'+79001234567')
                self.check('voice dial call type voice', from_tlv(0x10) == b'\x00')
                self.calls[1] = [1, 1, num.decode()]
                self.later(0.2, self.set_state, 1, 5)
                self.later(0.4, self.set_state, 1, 3)
                return OK + tlv(0x10, b'\x01')
            if msg == 0x0021:
                cid_ = from_tlv(0x01)[0]
                self.calls.pop(cid_, None)
                self.push_calls()
                if cid_ == 1:
                    self.later(0.5, self.incoming)
                return OK
            if msg == 0x0022:
                self.check('voice answer incoming call 2', from_tlv(0x01) == b'\x02')
                self.calls[2][0] = 3
                self.push_calls()
                return OK
            if msg == 0x0025:
                self.check('voice dtmf start call+digit', from_tlv(0x01) == b'\x01' + b'5')
                return OK
            if msg == 0x0026: return OK
        if svc == 1:   # WDS
            if msg == 0x004D: return OK
            if msg == 0x00A2:
                ep = from_tlv(0x10)
                self.check('wds bind mux data port: endpoint BAM_DMUX(5), 8 bytes', ep is not None and len(ep) == 8 and ep[:4] == b'\x05\x00\x00\x00')
                return OK
            if msg == 0x0020:
                self.check('wds apn', from_tlv(0x14) == b'internet.test')
                self.check('wds ip family 4', from_tlv(0x19) == b'\x04')
                self.check('wds user/pass', from_tlv(0x17) == b'user' and from_tlv(0x18) == b'pass')
                self.data_up = True
                self.later(0.2, self.ind, 1, 0x0022, tlv(0x01, b'\x02\x00'))
                return OK + tlv(0x01, struct.pack('<I', self.data_handle))
            if msg == 0x002D:
                return (OK + tlv(0x1E, ip_wire([10, 20, 30, 40])) + tlv(0x20, ip_wire([10, 20, 30, 1])) +
                        tlv(0x21, ip_wire([255, 255, 255, 0])) + tlv(0x15, ip_wire([8, 8, 8, 8])) +
                        tlv(0x16, ip_wire([8, 8, 4, 4])) + tlv(0x29, struct.pack('<I', 65536)))
            if msg == 0x0021:
                self.check('wds stop uses the handle from start', from_tlv(0x01) == struct.pack('<I', self.data_handle))
                self.data_up = False
                self.seen.add('wds_stop')
                self.later(0.1, self.ind, 1, 0x0022, tlv(0x01, b'\x01\x00'))
                return OK
        if svc == 26 and msg == 0x0021:
            return OK + tlv(0x11, struct.pack('<I', 2))
        return ERR(0x0047)

    def incoming(self):
        self.calls[2] = [2, 2, '+79001234567']
        self.push_calls()

    @staticmethod
    def tlvs(body):
        o = 0
        while o + 3 <= len(body):
            t, ln = struct.unpack_from('<BH', body, o)
            yield t, body[o + 3:o + 3 + ln]
            o += 3 + ln

    def serve(self):
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
                try:
                    rsp = self.handle(svc, cid, tx, msg, body)
                except Exception as e:  # a modem-side bug must fail the run, not hang it
                    self.check('modem handler exception %r (svc %d msg %#x)' % (e, svc, msg), False)
                    rsp = ERR(0x0003)
                with self.lock:
                    os.write(self.m, frame(svc, cid if svc else 0, 'rsp', tx, msg, rsp))


def main():
    argv = sys.argv[1:]
    argv = argv[1:] if argv and argv[0] == '--' else argv
    master, slave = pty.openpty()
    tty.setraw(slave, termios.TCSANOW)
    dev = os.ttyname(slave)
    mo = Modem(master)
    threading.Thread(target=mo.serve, daemon=True).start()
    p = subprocess.run([a.replace('{DEV}', dev) for a in argv], capture_output=True, text=True, timeout=90)
    time.sleep(0.2)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    bad = [n for n, ok in mo.checks if not ok]
    mo.check('ack for the MT SMS was received by the modem', 'ack' in mo.seen)
    mo.check('data call was stopped', 'wds_stop' in mo.seen)
    if os.environ.get('FAKE_UIM_UNPROVISIONED') == '1': mo.check('provisioning requested exactly once', mo.prov_requests == 1)
    bad = [n for n, ok in mo.checks if not ok]
    print('\n[fake modem] side checks: %d, failed: %d' % (len(mo.checks), len(bad)))
    for n in bad:
        print('  FAIL', n)
    return 1 if (p.returncode or bad) else 0


if __name__ == '__main__':
    sys.exit(main())
