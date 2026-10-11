"""wer.py voice.wav gain [--tts dir]: split the voice render into its 8
lines, transcribe each with whisper.cpp, score word error rate."""
import sys, os, re, subprocess, numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
LYR = ["Out here in the bush, under southern stars", "Solar on the roof, and the code runs far",
       "The didge breathes deep, and the land breathes too", "We're coding with AI in the morning dew",
       "Catch the rain, grow the food, live soft on the ground", "Old land, new minds, in the one same sound",
       "Respect to the people, this is native title land", "Wires in the gum trees, dust on our hands"]
W = os.path.expanduser('~/whisper/whisper.cpp')
norm = lambda s: re.sub(r"[^a-z' ]", ' ', s.lower().replace("we're", 'we are').replace('a i', 'ai').replace('a.i.', 'ai')).split()
def wer(ref, hyp):
    r, h = norm(ref), norm(hyp); d = list(range(len(h) + 1))
    for i in range(1, len(r) + 1):
        p, d[0] = d[0], i
        for j in range(1, len(h) + 1):
            p, d[j] = d[j], min(d[j] + 1, d[j - 1] + 1, p + (r[i - 1] != h[j - 1]))
    return d[len(h)] / len(r)
def transcribe(x, sr, tag):
    y = resample_poly(x, 1, 3) if sr == 48000 else x
    y = y / (np.abs(y).max() + 1e-9) * 0.9
    f = f'/tmp/wer_{os.getpid()}_{tag}.wav'; wavfile.write(f, 16000, (y * 32767).astype(np.int16))
    out = subprocess.run([W + '/build/bin/whisper-cli', '-m', W + '/models/ggml-small.en.bin', '-f', f, '-nt', '-np', '-t', '6', '-l', 'en'],
                         capture_output=True, text=True).stdout
    return ' '.join(out.split())
lines = []
if sys.argv[1] == '--tts':
    for k in range(8):
        sr, x = wavfile.read(f'{sys.argv[2]}/line{k + 1}.wav'); x = x.astype(float)
        if sr != 16000: x = resample_poly(x, 16000, sr)
        lines.append((x, 16000))
else:
    sr, x = wavfile.read(sys.argv[1]); x = x.astype(float)
    bar = 4 * 60 / 108
    for k in range(8):
        a = int(4 * k * bar * sr); b = int((4 * k + 3) * bar * sr)
        lines.append((x[a:b], sr))
# Whisper flips on small changes; average three lead-ins of silence.
PADS = [0.0, 0.13, 0.27]
tot = 0
for k, (x, sr) in enumerate(lines):
    es, hyps = [], []
    for j, pad in enumerate(PADS):
        xp = np.concatenate([np.zeros(int(pad * sr)), x])
        hyp = transcribe(xp, sr, f'{k}_{j}'); es.append(wer(LYR[k], hyp)); hyps.append(hyp)
    e = sum(es) / len(es); tot += e
    print(f'{e*100:4.0f}%  {LYR[k]!r:58} -> {hyps[0]!r}')
print(f'MEAN WER {tot / 8 * 100:.0f}%')
