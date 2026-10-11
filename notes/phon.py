"""phon.py: sung lyrics -> aitherLang score arrays.

A line is syllables separated by ' / '; a syllable is phonemes as
NAME:slots and one @note (semitones above D3). A slot is a 32nd note
(69 ms at 108 bpm); a line has 96 (3 bars). Out come `lyr` (the phoneme
in each slot) and `lyrN` (the note), 96 per line."""
import sys
IDS = 'sil IY IH EH AE AA AO UH UW AH ER AX M N NG L R W Y HH S SH F TH Z V DH ZH P T K B D G CH JH VB'.split()
STOPS = {'P': 'sil', 'T': 'sil', 'K': 'sil', 'CH': 'sil', 'B': 'VB', 'D': 'VB', 'G': 'VB', 'JH': 'VB'}
SLOTS = 96
LINES = [
 ("Out here in the bush, under southern stars",
  "AA:5 UW:2 T:1 @7 / HH:1 IY:4 AX:1 R:2 @7 / IH:3 N:2 @5 / DH:1 AX:2 @3 / B:1 UH:5 SH:3 @5 / sil:3 @5 / AH:4 N:2 @7 / D:1 ER:4 @5 / S:2 AH:5 @3 / DH:1 ER:3 N:2 @0 / S:2 T:1 AA:7 R:2 Z:3 @3"),
 ("Solar on the roof, and the code runs far",
  "S:2 AO:4 UW:2 @7 / L:1 ER:4 @5 / AA:3 N:2 @7 / DH:1 AX:2 @5 / R:1 UW:5 F:3 @3 / sil:3 @3 / AE:3 N:1 D:1 @5 / DH:1 AX:2 @7 / K:1 AO:4 UW:2 D:1 @10 / R:1 AH:4 N:2 Z:2 @7 / F:2 AA:8 R:3 @7"),
 ("The didge breathes deep, and the land breathes too",
  "DH:1 AX:2 @7 / D:1 IH:4 JH:2 @10 / B:1 R:1 IY:5 DH:1 Z:2 @12 / D:1 IY:5 P:2 @10 / sil:3 @10 / AE:3 N:1 D:1 @7 / DH:1 AX:2 @7 / L:2 AE:5 N:2 D:1 @10 / B:1 R:1 IY:5 DH:1 Z:2 @7 / T:1 UW:9 @7"),
 ("We're coding with AI in the morning dew",
  "W:2 IY:3 ER:3 @7 / K:1 AO:4 UW:2 @7 / D:1 IH:3 NG:2 @5 / W:1 IH:3 TH:2 @3 / EH:4 IY:2 @7 / AA:5 IY:3 @10 / IH:3 N:1 @7 / DH:1 AX:2 @5 / M:2 AO:5 R:2 @7 / N:1 IH:3 NG:2 @5 / D:1 Y:1 UW:9 @3"),
 ("Catch the rain, grow the food, live soft on the ground",
  "K:1 AE:4 CH:2 @0 / DH:1 AX:2 @3 / R:1 EH:4 IY:2 N:2 @5 / sil:2 @5 / G:1 R:1 AO:4 UW:2 @7 / DH:1 AX:2 @5 / F:2 UW:5 D:1 @3 / sil:2 @3 / L:2 IH:4 V:2 @7 / S:2 AO:5 F:2 T:1 @10 / AA:3 N:1 @7 / DH:1 AX:2 @5 / G:1 R:1 AA:4 UW:3 N:2 D:1 @3"),
 ("Old land, new minds, in the one same sound",
  "AO:4 UW:2 L:2 D:1 @12 / L:2 AE:5 N:2 D:1 @10 / sil:2 @10 / N:2 UW:6 @7 / M:2 AA:4 IY:2 N:2 D:1 Z:2 @10 / sil:2 @10 / IH:3 N:1 @7 / DH:1 AX:2 @5 / W:2 AH:4 N:2 @7 / S:2 EH:4 IY:2 M:3 @10 / S:2 AA:4 UW:3 N:2 D:1 @12"),
 ("Respect to the people, this is native title land",
  "R:1 IH:2 @7 / S:2 P:1 EH:4 K:1 T:1 @10 / T:1 UW:3 @7 / DH:1 AX:2 @5 / P:1 IY:5 @7 / P:1 AX:2 L:2 @5 / sil:2 @5 / DH:1 IH:3 S:2 @7 / IH:3 Z:2 @5 / N:2 EH:4 IY:2 @7 / T:1 IH:3 V:2 @5 / T:1 AA:4 IY:2 @7 / T:1 AX:2 L:2 @5 / L:2 AE:6 N:2 D:1 @3"),
 ("Wires in the gum trees, dust on our hands",
  "W:2 AA:4 IY:1 AX:1 ER:2 Z:2 @7 / IH:3 N:1 @5 / DH:1 AX:2 @3 / G:1 AH:4 M:2 @5 / T:1 R:1 IY:5 Z:2 @7 / sil:3 @7 / D:1 AH:4 S:2 T:1 @10 / AA:3 N:1 @7 / AA:3 UW:2 ER:2 @5 / HH:1 AE:6 N:2 D:1 Z:2 @3"),
]
out = ['# The words: syllables (phoneme:32nds @note above D3), 3 bars a line.']
score, notes = [], []
for words, line in LINES:
    slots, ns = [], []
    for syl in line.split(' / '):
        toks = syl.split(); note = int([t for t in toks if t.startswith('@')][0][1:])
        for tok in toks:
            if tok.startswith('@'): continue
            p, n = tok.split(':'); n = int(n)
            # a long stop holds one closure (silence, or a voice bar) and releases once, at the end
            seq = [STOPS[p]] * (n - 1) + [p] if p in STOPS and n > 1 else [p] * n
            slots += [IDS.index(q) for q in seq]; ns += [note] * n
    assert len(slots) <= SLOTS, (words, len(slots))
    last = ns[-1]
    slots += [0] * (SLOTS - len(slots)); ns += [last] * (SLOTS - len(ns))
    score += slots; notes += ns
    out.append(f'#   {words}')
    out.append(f'#     {line}')
for name, arr in (('lyr', score), ('lyrN', notes)):
    out.append(f'let {name} = [')
    for i in range(0, len(arr), SLOTS):
        out.append('  ' + ', '.join(map(str, arr[i:i + SLOTS])) + (',' if i + SLOTS < len(arr) else ''))
    out.append(']')
print('\n'.join(out))
