import sys
d=open(sys.argv[1],'rb').read(); out=bytearray(); i=0; br=0
while i+188<=len(d):
    if d[i]==0x47 and (i+188>=len(d) or d[i+188]==0x47):
        out+=d[i:i+188]; i+=188; continue
    br+=1; j=i+1
    while j+376<len(d) and not (d[j]==0x47 and d[j+188]==0x47 and d[j+376]==0x47): j+=1
    i=j
open(sys.argv[2],'wb').write(out); print("packets",len(out)//188,"resyncs",br)
