# Emulate kernel lgdt3305.c (demod_chip=LGDT3304) VSB register sequence and test lock + TS
import sys, time
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skytv
K3304_INIT = [(0x0000,0x03),(0x000d,0x02),(0x000e,0x02),(0x0012,0x32),(0x0013,0xc4),
 (0x0106,0),(0x0107,0),(0x0108,0),(0x0109,0),(0x0126,0xf9),(0x0112,0x17),(0x0113,0x15),
 (0x0114,0x18),(0x0115,0xff),(0x0116,0x3c),(0x0214,0x67),(0x0424,0x8d),(0x0427,0x12),
 (0x0428,0x4f),(0x0308,0x80),(0x0309,0x00),(0x030a,0x08),(0x030b,0x9b),(0x030d,0x00),
 (0x030e,0x1c),(0x0314,0xe1),(0x000d,0x82),(0x050e,0x5b),(0x050e,0x5b)]
def setbit(s,reg,bit,on):
    v=s.dr(reg); v=(v&~(1<<bit))|((on&1)<<bit); s.dw(reg,v)
def k_init(s):
    for r,v in K3304_INIT: s.dw(r,v)
    s.demod_soft_reset()
def k_set_params(s, freq, edge=1, clkmode=0, valid=1, inv=0, fixed_nco=True):
    s.xc_tune(freq)
    s.dw(0x0000,(s.dr(0x0000)&~3)|3)
    s.dw(0x0012,0x32); s.dw(0x0013,0xc4)
    s.dw(0x0314,0xe1); setbit(s,0x030e,2,0)
    s.dw(0x0308,0x80); s.dw(0x0309,0x00)
    s.dw(0x030d,0x00)
    if fixed_nco:
        for r,v in zip((0x0106,0x0107,0x0108,0x0109),(0x4f,0x0c,0xac,0xba)): s.dw(r,v)
    s.dw(0x0126,0xf9 if inv else 0x79)
    setbit(s,0x050e,5,0)                     # parallel
    v=s.dr(0x050e)&~0x09
    v|=(0x08 if edge else 0)|(0x40 if clkmode else 0)|(0x01 if valid else 0)
    s.dw(0x050e,v)
    s.demod_soft_reset()
s=skytv.Sky(); s.power_on(); s.xc_load_fw()
f=skytv.ch_freq(int(sys.argv[1]) if len(sys.argv)>1 else 15)
k_init(s); k_set_params(s,f,fixed_nco=(len(sys.argv)<3))
# bridge override after kernel set_frontend
for r in (0x0106,0x0107,0x0108,0x0109): s.dw(r,0)
s.dw(0x050e,0x1b); s.demod_soft_reset()
for i in range(30):
    st=s.demod_status()
    if st['sync']: break
    time.sleep(0.1)
print("kernel-path:",st,"mse",s.demod_mse(),"050e=%02x"%s.dr(0x050e))
s.stream_start(ext_clk=False); d=s.read_ts(2*19392658//8); s.stream_stop()
n=sum(1 for i in range(0,len(d)-188) if d[i]==0x47 and d[i+188]==0x47)
print("bytes",len(d),"aligned pkts ~",n)
