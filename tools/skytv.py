#!/usr/bin/env python3
# SkyTV HD USB mini (1c6c:04c9) userspace test driver
# FX2 (CY7C68013) + LGDT3304 (@0x59) + XC5000 (@0x61) + CX25843 (@0x44)
import sys, time, argparse
import usb.core, usb.util

VID, PID = 0x1c6c, 0x04c9
import os
FX2_FW_CANDIDATES = [
    os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'dvb-usb-skytv-hdusbmini.fw'),
    '/lib/firmware/dvb-usb-skytv-hdusbmini.fw',
]
FX2_FW = next((p for p in FX2_FW_CANDIDATES if os.path.exists(p)), FX2_FW_CANDIDATES[-1])
XC_FW = '/lib/firmware/dvb-fe-xc5000-1.6.114.fw'
DEMOD, TUNER = 0x59, 0x61
IF_KHZ = 6000

# Windows driver LGDT3304 tables (VA 0x34450 / 0x344d4 / 0x3448c / 0x344b0)
VSB_INIT = [(0x0000,0x03),(0x000d,0x02),(0x000e,0x02),(0x0012,0x32),(0x0013,0xc4),
            (0x0112,0x17),(0x0113,0x15),(0x0114,0x18),(0x0115,0xff),(0x0116,0xbc),
            (0x0126,0x79),(0x0214,0x67),(0x0424,0x8d),(0x0427,0x12),(0x0428,0x4f),
            (0x0308,0x80),(0x0309,0x00),(0x030d,0x14),(0x030e,0x1c),(0x0314,0xe1)]
QAM_INIT = lambda q256, inv: [(0x0000,0x01 if q256 else 0x00),(0x000d,0x02),(0x0012,0x2a),
            (0x0013,0x80 if q256 else 0x00),(0x0314,0xe3),(0x030e,0x1c),(0x0308,0x66),
            (0x0309,0x66),(0x030a,0x08),(0x030b,0x9b),(0x030d,0x14),(0x0504,0xfd if inv else 0xff)]


class Sky:
    def __init__(self):
        self.d = usb.core.find(idVendor=VID, idProduct=PID)
        if self.d is None:
            sys.exit("device not found")
        if not self._fw_running():
            self._load_fx2()
        try:
            if self.d.is_kernel_driver_active(0):
                self.d.detach_kernel_driver(0)
        except Exception:
            pass
        self.d.set_interface_altsetting(0, 0)   # EP2 IN bulk
        self._drain()
        self.oe = {0: 0, 3: 0}
        self.val = {0: self.port_read(0), 3: self.port_read(3)}

    # ---- FX2 ----
    def _fw_running(self):
        try:
            return usb.util.get_string(self.d, self.d.iManufacturer) == 'SKYDIGITAL'
        except Exception:
            return False

    def _load_fx2(self):
        fw = open(FX2_FW, 'rb').read()
        w = lambda a, data: self.d.ctrl_transfer(0x40, 0xA0, a, 0, data)
        w(0xE600, b'\x01')
        for a in range(0, len(fw), 64):
            w(a, fw[a:a+64])
        w(0xE600, b'\x00')
        usb.util.dispose_resources(self.d)
        for _ in range(50):
            time.sleep(0.2)
            self.d = usb.core.find(idVendor=VID, idProduct=PID)
            if self.d is not None and self._fw_running():
                print("FX2 firmware loaded")
                return
        sys.exit("FX2 did not re-enumerate")

    def _drain(self):
        for _ in range(4):
            try:
                self.d.read(0x81, 64, timeout=50)
            except Exception:
                pass

    def cmd(self, b, reply=True):
        self.d.write(0x01, bytes(b), timeout=1000)
        if not reply:
            return None
        for attempt in range(4):
            try:
                return bytes(self.d.read(0x81, 64, timeout=500))
            except usb.core.USBTimeoutError:
                self._drain()
                self.d.write(0x01, bytes(b), timeout=1000)
        raise IOError(f"no reply to cmd {bytes(b[:4]).hex()}")

    def port_read(self, p):
        return self.cmd([0x0d, p])[0]

    def gpio(self, p, bit, v):
        self.oe[p] |= 1 << bit
        self.val[p] = (self.val[p] | (1 << bit)) if v else (self.val[p] & ~(1 << bit))
        self.cmd([0x0e, p, self.oe[p], self.val[p]])

    # ---- I2C (7-bit addr); status 8 = ok ----
    def i2c_w(self, addr, data):
        r = self.cmd([0x08, addr, len(data)] + list(data))
        if r[0] != 8:
            raise IOError(f"i2c write 0x{addr:02x} failed st={r[0]}")

    def i2c_wr(self, addr, wdata, n):
        r = self.cmd([0x09, len(wdata), n, addr] + list(wdata))
        if r[0] != 8:
            raise IOError(f"i2c read 0x{addr:02x} failed st={r[0]}")
        return r[1:1+n]

    # ---- power ----
    def power_on(self):
        self.gpio(3, 6, 1)
        self.gpio(3, 1, 1)          # tuner out of reset
        time.sleep(0.05)
        self.demod_reset()

    def demod_reset(self):
        self.gpio(3, 7, 0); time.sleep(0.01); self.gpio(3, 7, 1); time.sleep(0.05)

    def tuner_reset(self):
        self.gpio(3, 1, 0); time.sleep(0.01); self.gpio(3, 1, 1); time.sleep(0.01)

    # ---- LGDT3304 ----
    def dw(self, reg, v):
        self.i2c_w(DEMOD, [reg >> 8, reg & 0xff, v])

    def dr(self, reg):
        return self.i2c_wr(DEMOD, [reg >> 8, reg & 0xff], 1)[0]

    def demod_soft_reset(self):
        v = self.dr(0x0002)
        self.dw(0x0002, v & ~1); time.sleep(0.02); self.dw(0x0002, v | 1)

    def demod_setup(self, mod='vsb', inv=False):
        if mod == 'vsb':
            tbl = [(r, (0xf9 if inv else 0x79) if r == 0x0126 else v) for r, v in VSB_INIT]
        else:
            tbl = QAM_INIT(mod == 'qam256', inv)
        for r, v in tbl:
            self.dw(r, v)
        self.demod_soft_reset()

    def demod_status(self):
        g = self.dr(0x0003); cr = self.dr(0x011d)
        return dict(signal=bool(g & 0x10), inlock=not (g & 0x08), sync=bool(g & 0x04),
                    nofecerr=bool(g & 0x02), snrgood=bool(g & 0x01),
                    cr_vsb=bool(cr & 0x80), raw=(g, cr))

    def demod_mse(self):
        b = [self.dr(r) for r in (0x0413, 0x0414, 0x0415)]
        return ((b[0] & 0x0f) << 16) | (b[1] << 8) | b[2]

    # ---- XC5000 ----
    def xc_w(self, reg, val):
        self.i2c_w(TUNER, [reg >> 8, reg & 0xff, val >> 8, val & 0xff])
        for _ in range(100):
            if self.xc_r(0x09) == 0:
                return
            time.sleep(0.005)

    def xc_r(self, reg):
        b = self.i2c_wr(TUNER, [reg >> 8, reg & 0xff], 2)
        return (b[0] << 8) | b[1]

    def xc_load_fw(self, force=False):
        if not force and self.xc_r(0x08) == 0x1388:
            return
        seq = open(XC_FW, 'rb').read()
        i = 0
        CH = 48
        while not (seq[i] == 0xff and seq[i+1] == 0xff):
            ln = (seq[i] << 8) | seq[i+1]
            if ln == 0:
                self.tuner_reset(); i += 2
            elif ln & 0x8000:
                time.sleep((ln & 0x7fff) / 1000); i += 2
            else:
                i += 2
                hdr = seq[i:i+2]; pos = 2
                while pos < ln:
                    n = min(CH, ln - pos)
                    self.i2c_w(TUNER, list(hdr) + list(seq[i+pos:i+pos+n]))
                    pos += n
                i += ln
        self.xc_w(0x00, 0)          # XREG_INIT
        time.sleep(0.1)
        pid = self.xc_r(0x08)
        ver = self.xc_r(0x07)
        print(f"xc5000 fw loaded: product 0x{pid:04x} ver {ver>>12}.{(ver>>8)&0xf} build {self.xc_r(0x0d)}")

    def xc_tune(self, freq_hz, cable=False):
        self.xc_w(0x0d, 1 if cable else 0)        # signal source
        self.xc_w(0x01, 0x8002); self.xc_w(0x02, 0x00c0)  # DTV6 video/audio
        self.xc_w(0x05, IF_KHZ * 1024 // 1000)    # IF out
        self.xc_w(0x0b, 0x8a)                     # output amp
        self.xc_w(0x10, (freq_hz - 1750000) // 15625)

    def xc_lock(self):
        return self.xc_r(0x04)

    # ---- streaming ----
    def stream_start(self, ext_clk=True):
        self.cmd([0x36, 0x03, 0x00, 0x00 if ext_clk else 0x01], reply=False)

    def stream_stop(self):
        self.cmd([0x37], reply=False)

    def read_ts(self, nbytes, timeout=2000):
        out = bytearray()
        t0 = time.time()
        while len(out) < nbytes and time.time() - t0 < timeout / 1000 * 4:
            try:
                out += bytes(self.d.read(0x82, 512 * 64, timeout=timeout))
            except usb.core.USBTimeoutError:
                break
        return bytes(out)


def ts_check(data):
    if len(data) < 188 * 4:
        return 0.0, -1
    best = (0, -1)
    for off in range(188):
        n = sum(1 for k in range(off, len(data) - 188, 188) if data[k] == 0x47)
        best = max(best, (n, off))
    total = (len(data) - best[1]) // 188
    return best[0] / max(total, 1), best[1]


def ch_freq(ch):
    # North America / Korea 6 MHz plan
    if 2 <= ch <= 4: return (57 + 6 * (ch - 2)) * 1000000
    if 5 <= ch <= 6: return (79 + 6 * (ch - 5)) * 1000000
    if 7 <= ch <= 13: return (177 + 6 * (ch - 7)) * 1000000
    if 14 <= ch <= 69: return (473 + 6 * (ch - 14)) * 1000000
    raise ValueError(ch)


def tune(s, ch, mod='vsb', inv=False, wait=1.5):
    f = ch_freq(ch)
    s.demod_setup(mod, inv)
    s.xc_tune(f, cable=(mod != 'vsb'))
    s.demod_soft_reset()
    t0 = time.time()
    st = s.demod_status()
    while time.time() - t0 < wait and not st['sync']:
        time.sleep(0.1)
        st = s.demod_status()
    return f, st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scan', action='store_true')
    ap.add_argument('--ch', type=int)
    ap.add_argument('--inv', action='store_true')
    ap.add_argument('--mod', default='vsb')
    ap.add_argument('--out')
    ap.add_argument('--secs', type=float, default=5)
    ap.add_argument('--intclk', action='store_true')
    a = ap.parse_args()

    s = Sky()
    s.power_on()
    s.xc_load_fw()

    if a.scan:
        found = []
        for ch in list(range(14, 52)) + list(range(2, 14)):
            for inv in (False, True):
                f, st = tune(s, ch, 'vsb', inv, wait=1.2)
                if st['signal'] or st['cr_vsb'] or st['sync']:
                    print(f"ch{ch:2d} {f/1e6:.0f}MHz inv={int(inv)} {st} xclock={s.xc_lock()} mse={s.demod_mse()}")
                if st['sync']:
                    found.append((ch, inv)); break
        print("locked channels:", found)
        return

    if a.ch:
        f, st = tune(s, a.ch, a.mod, a.inv, wait=3)
        print(f"ch{a.ch} {f/1e6:.0f}MHz {st} xclock={s.xc_lock()} mse={s.demod_mse()}")
        s.stream_start(ext_clk=not a.intclk)
        data = s.read_ts(int(19392658 / 8 * a.secs))
        s.stream_stop()
        ratio, off = ts_check(data)
        print(f"read {len(data)} bytes, sync ratio {ratio:.3f} at offset {off}")
        if a.out:
            open(a.out, 'wb').write(data[off if off > 0 else 0:])
            print("saved", a.out)


if __name__ == '__main__':
    main()
