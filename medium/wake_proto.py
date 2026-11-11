# The wake's note from wake_proto at each rate, split and lag.
import subprocess, sys, numpy as np
sys.path.insert(0, '../tests/physics'); import run
for how in ('kick', 'split', 'lag', 'lag-euler'):
    fs = []
    for rate in (48000, 96000, 192000, 384000):
        p = subprocess.run(['./wake_proto', str(rate), '4', how], capture_output=True)
        a = np.frombuffer(p.stdout, dtype=np.float32).astype(np.float64)
        fs.append(run.strongest(a[1::2][int(2 * rate):], rate))
    off = max(abs(run.cents(f, fs[-1])) for f in fs[:-1])
    print('%-5s ' % how + ' '.join('%.3f' % f for f in fs) + ' Hz at 48, 96, 192, 384 kHz: %.2f cents from the finest' % off)
