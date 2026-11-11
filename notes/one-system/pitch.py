import sys,array,math,subprocess,os,cmath
def run(patch,rate,sec=2.5):
    env=dict(os.environ,AITHER_RATE=str(rate))
    p=subprocess.run(['/home/roland/aitherLang/aither',patch,str(sec)],stdin=subprocess.DEVNULL,capture_output=True,env=env)
    err=[l for l in p.stderr.decode(errors='replace').split('\n') if 'rror' in l or 'exceed' in l]
    a=array.array('f');a.frombytes(p.stdout);return a[1::2],err
def f0(x,rate,t0=1.0,t1=2.0):
    d=rate//12000; y=[sum(x[i:i+d])/d for i in range(int(t0*rate),int(t1*rate),d)]; r=rate/d
    m=sum(y)/len(y); y=[v-m for v in y]
    best=max(range(int(r/250),int(r/40)),key=lambda L:sum(y[i]*y[i+L] for i in range(len(y)-L)))
    f=r/best; n=len(y); w=[v*(.5-.5*math.cos(2*math.pi*i/n)) for i,v in enumerate(y)]
    def amp(fr):
        st=cmath.exp(-2j*math.pi*fr/r);z=1;s=0
        for v in w: s+=v*z;z*=st
        return abs(s)
    fs=[f*(1+(k-100)*0.0004) for k in range(201)]
    return max(fs,key=amp), math.sqrt(sum(v*v for v in y)/n)
for patch in sys.argv[1:]:
    for rate in (48000,96000,192000):
        x,err=run(patch,rate)
        if err: print(os.path.basename(patch),rate,err[0]); continue
        f,rms=f0(x,rate)
        print('%-16s %6d Hz: sounds %.3f Hz, bore rms %.4f'%(os.path.basename(patch),rate,f,rms))
