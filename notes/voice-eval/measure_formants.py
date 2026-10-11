import sys, numpy as np, parselmouth
from scipy.io import wavfile
sr, x = wavfile.read(sys.argv[1]); x = x.astype(float) / 32768
names = ["IY","IH","EH","AE","AA","AO","UH","UW","AH","ER","AX"]
tg = {"IY":(359,2381,2929),"IH":(448,1953,2641),"EH":(603,1774,2586),"AE":(634,1854,2530),"AA":(774,1350,2434),"AO":(679,1061,2436),
      "UH":(480,1227,2432),"UW":(383,976,2373),"AH":(618,1274,2520),"ER":(457,1383,1705),"AX":(500,1500,2500)}
snd = parselmouth.Sound(x, sampling_frequency=sr)
fm = snd.to_formant_burg(time_step=0.01, max_number_of_formants=5, maximum_formant=5000)
err = []
for i, n in enumerate(names):
    vals = [np.nanmedian([fm.get_value_at_time(k, i + 0.3 + 0.04 * j) for j in range(10)]) for k in (1, 2, 3)]
    e = [abs(np.log(v / t)) for v, t in zip(vals, tg[n])]; err.append(e)
    print(f"{n:3s} got {vals[0]:5.0f} {vals[1]:5.0f} {vals[2]:5.0f}   want {tg[n][0]:4d} {tg[n][1]:5d} {tg[n][2]:5d}")
e = np.array(err); print("median error %%: F1 %.0f F2 %.0f F3 %.0f" % tuple(np.median(e, 0) * 100))
