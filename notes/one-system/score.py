import sys,array,math,subprocess,os
fa,fb=440.0,466.16
def run(patch,rate):
    env=dict(os.environ,AITHER_RATE=str(rate))
    p=subprocess.run(['/home/roland/aitherLang/aither',patch,'2'],stdin=subprocess.DEVNULL,capture_output=True,env=env)
    a=array.array('f');a.frombytes(p.stdout);return a[0::2],a[1::2]
def mode(sig,f,rate,t0,t1):
    # project onto sin and cos at f: amplitude and phase
    i0,i1=int(t0*rate),int(t1*rate);s=c=0
    for i in range(i0,i1):
        t=i/rate;s+=sig[i]*math.sin(2*math.pi*f*t);c+=sig[i]*math.cos(2*math.pi*f*t)
    n=i1-i0;return 2*math.hypot(s,c)/n, math.atan2(c,s)
for patch in sys.argv[1:]:
    for rate in (48000,96000,192000):
        L,R=run(patch,rate)
        out=[]
        for name,f,sig,want in (('mode 1 (440)',fa,[l+r for l,r in zip(L,R)],2*math.pi*440/(2*math.pi*fa)),('mode 2 (466)',fb,[l-r for l,r in zip(L,R)],2*math.pi*440/(2*math.pi*fb))):
            a0,p0=mode(sig,f,rate,0.0,0.1);a1,p1=mode(sig,f,rate,1.9,2.0)
            dphase=(p1-p0+math.pi)%(2*math.pi)-math.pi     # phase gained over 1.9 s against the exact frequency
            cents=1200*math.log2(1+dphase/(2*math.pi*f*1.9))
            out.append('%s %+7.3f cents, level %+.4f dB over 2 s'%(name,cents,20*math.log10(a1/a0)))
        print('%-12s %6d Hz  '%(os.path.basename(patch),rate)+' | '.join(out))
