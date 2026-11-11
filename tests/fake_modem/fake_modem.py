#!/usr/bin/env python3
"""Independent (Python) fake QMUX modem on a pty, used to exercise qmi-probe.

It is written from the public QMI conventions, NOT from the C codec, so a
framing mistake in the C code would show up as a mismatch here. It still only
proves self-consistency with those conventions - the real modem is the judge.

usage: fake_modem.py [--sim absent|present] -- <command using {DEV}> ...
"""
import os, pty, struct, subprocess, sys, termios, threading, tty

def tlv(t, v): return struct.pack('<BH', t, len(v)) + v
OK = tlv(0x02, struct.pack('<HH', 0, 0))

def frame(svc, cid, kind, tx, msg, body):
    if svc == 0:
        sdu = struct.pack('<BBHH', {'rsp': 1, 'ind': 2, 'req': 0}[kind], tx & 0xFF, msg, len(body))
    else:
        sdu = struct.pack('<BHHH', {'rsp': 2, 'ind': 4, 'req': 0}[kind], tx, msg, len(body))
    payload = struct.pack('<BBB', 0x80, svc, cid) + sdu + body
    # QMUX length counts every byte after the marker, INCLUDING these 2 bytes
    # (real capture: 28-byte frame carries 0x1B = 27).
    return b'\x01' + struct.pack('<H', len(payload) + 2) + payload

def parse(buf):
    if buf[0] != 1: raise ValueError('bad marker')
    (ln,) = struct.unpack_from('<H', buf, 1)
    if len(buf) < ln + 1: return None
    flags, svc, cid = buf[3], buf[4], buf[5]
    if svc == 0:
        _, tx, msg, tl = struct.unpack_from('<BBHH', buf, 6); hdr = 12
    else:
        _, tx, msg, tl = struct.unpack_from('<BHHH', buf, 6); hdr = 13
    return ln + 1, (flags, svc, cid, tx, msg, buf[hdr:hdr + tl])

def card_status(present, provisioned=True):
    if present and not provisioned:   # as seen on the real modem: app only "detected", indices 0xFFFF
        app = bytes([2, 1, 0, 0, 0, 0, 16]) + bytes.fromhex('a0000000871002ff47f0018900000 1ff'.replace(' ', '')) + bytes([0] * 7)
        return struct.pack('<HHHHB', 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 1) + bytes([1, 0, 0, 0, 0, 1]) + app
    if not present:
        return struct.pack('<HHHHB', 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 1) + bytes([0, 0, 0, 0, 0, 0])
    app = bytes([2, 7, 0, 0, 0, 0, 7]) + bytes.fromhex('a0000000871002') + bytes([0, 1, 3, 10, 1, 3, 10])
    return struct.pack('<HHHHB', 0, 0xFFFF, 0xFFFF, 0xFFFF, 1) + bytes([1, 0, 0, 0, 0, 1]) + app

def serve(master, sim_present, log):
    cids = {}; buf = b''
    while True:
        try: data = os.read(master, 4096)
        except OSError: return
        if not data: return
        buf += data
        while buf:
            r = parse(buf)
            if r is None: break
            n, (flags, svc, cid, tx, msg, body) = r; buf = buf[n:]
            log.append((svc, msg))
            rsp = None
            if svc == 0 and msg == 0x0021:
                ents = [(1, 1, 0), (2, 1, 0), (3, 1, 0), (11, 1, 0)]
                rsp = OK + tlv(0x01, bytes([len(ents)]) + b''.join(struct.pack('<BHH', *e) for e in ents))
            elif svc == 0 and msg == 0x0022:
                s = body[3]; cids[s] = 0x10 + s; rsp = OK + tlv(0x01, bytes([s, cids[s]]))
            elif svc == 0 and msg == 0x0023: rsp = OK + tlv(0x01, body[3:5])
            elif svc == 2 and msg == 0x0023: rsp = OK + tlv(0x01, b'MPSS.TH.2.0-FAKE')
            elif svc == 2 and msg == 0x0025: rsp = OK + tlv(0x11, b'351234567890123') + tlv(0x13, b'01')
            elif svc == 2 and msg == 0x002D: rsp = OK + tlv(0x01, b'\x01') + tlv(0x11, b'\x00')
            elif svc == 11 and msg == 0x002F: rsp = OK + tlv(0x10, card_status(sim_present))
            elif svc == 3 and msg == 0x0024: rsp = OK + tlv(0x01, bytes([2, 2, 2, 2, 1, 8]))
            elif svc == 3 and msg == 0x0020: rsp = OK + tlv(0x01, bytes([0xB0 & 0xFF, 8]))
            else: rsp = tlv(0x02, struct.pack('<HH', 1, 0x0047))
            os.write(master, frame(svc, cid if svc else 0, 'rsp', tx, msg, rsp))

def main():
    argv = sys.argv[1:]; sim = True
    if argv[:2] == ['--sim', 'absent']: sim = False; argv = argv[2:]
    elif argv[:2] == ['--sim', 'present']: argv = argv[2:]
    argv = argv[1:] if argv and argv[0] == '--' else argv
    master, slave = pty.openpty(); tty.setraw(slave, termios.TCSANOW)
    dev = os.ttyname(slave); log = []
    threading.Thread(target=serve, args=(master, sim, log), daemon=True).start()
    p = subprocess.run([a.replace('{DEV}', dev) for a in argv], capture_output=True, text=True, timeout=60)
    sys.stdout.write(p.stdout); sys.stderr.write(p.stderr)
    sys.stdout.write('\n[fake_modem] requests served: %d, exit code of command: %d\n' % (len(log), p.returncode))
    return p.returncode

if __name__ == '__main__':
    sys.exit(main())
