import sys, os, subprocess, numpy as np
AI = os.environ.get('AI', '/home/roland/aitherLang/aither')
for patch in sys.argv[1:]:
    for rate in [int(r) for r in os.environ.get("RATES", "48000 96000 192000").split()]:
        env = dict(os.environ, AITHER_RATE=str(rate))
        p = subprocess.run([AI, patch, '4'], stdin=subprocess.DEVNULL, capture_output=True, env=env)
        err = [l for l in p.stderr.decode(errors='replace').split('\n') if 'rror' in l or 'exceed' in l]
        a = np.frombuffer(p.stdout, dtype=np.float32).astype(np.float64)
        if err or len(a) == 0: print(os.path.basename(patch), rate, err[:1]); continue
        x = a[0::2][int(2 * rate):]; q = a[1::2][int(2 * rate):]
        w = np.hanning(len(q)); pad = 8
        S = np.abs(np.fft.rfft(q * w, len(q) * pad)); df = rate / (len(q) * pad)
        i = int(np.argmax(S[int(50 / df):int(3000 / df)])) + int(50 / df)
        a_, b_, c_ = np.log(S[i - 1:i + 2]); d = 0.5 * (a_ - c_) / (a_ - 2 * b_ + c_)
        f = (i + d) * df
        print('%-18s %6d Hz: the wake sings %.3f Hz (harmonic %.3f), string rms %.5f, wake rms %.4f' %
              (os.path.basename(patch), rate, f, f / 110, np.sqrt(np.mean(x * x)), np.sqrt(np.mean(q * q))))
