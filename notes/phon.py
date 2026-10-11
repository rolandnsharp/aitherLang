"""phon.py: lyrics written as phonemes:slots -> aitherLang score arrays.
48 slots (3 bars of 16ths) per line; a slot is one 16th note."""
IDS = 'sil IY IH EH AE AA AO UH UW AH ER AX M N NG L R W Y HH S SH F TH Z V DH ZH P T K B D G CH JH'.split()
LINES = [
 ("Out here in the bush, under southern stars",
  "AA:2 UW:1 T:1 HH:1 IY:3 ER:1 IH:2 N:1 DH:1 AX:1 B:1 UH:2 SH:2 sil:1 AH:2 N:1 D:1 ER:2 S:1 AH:2 DH:1 ER:1 N:1 S:1 T:1 AA:4 R:1 Z:1"),
 ("Solar on the roof, and the code runs far",
  "S:1 AO:2 UW:1 L:1 ER:2 AA:2 N:1 DH:1 AX:1 R:1 UW:3 F:2 sil:1 AE:1 N:1 D:1 DH:1 AX:1 K:1 AO:2 UW:1 D:1 R:1 AH:2 N:1 Z:1 F:1 AA:4 R:1"),
 ("The didge breathes deep, and the land breathes too",
  "DH:1 AX:1 D:1 IH:2 JH:1 B:1 R:1 IY:3 DH:1 Z:1 D:1 IY:3 P:1 sil:1 AE:1 N:1 D:1 DH:1 AX:1 L:1 AE:3 N:1 D:1 B:1 R:1 IY:2 DH:1 Z:1 T:1 UW:4"),
 ("We're coding with AI in the morning dew",
  "W:1 IY:1 ER:1 K:1 AO:2 UW:1 D:1 IH:1 NG:1 W:1 IH:1 TH:1 EH:2 IY:1 AA:2 IY:1 IH:1 N:1 DH:1 AX:1 M:1 AO:2 R:1 N:1 IH:1 NG:1 D:1 Y:1 UW:4"),
 ("Catch the rain, grow the food, live soft on the ground",
  "K:1 AE:2 CH:1 DH:1 AX:1 R:1 EH:2 IY:1 N:1 sil:1 G:1 R:1 AO:2 UW:1 DH:1 AX:1 F:1 UW:2 D:1 sil:1 L:1 IH:2 V:1 S:1 AO:2 F:1 T:1 AA:1 N:1 DH:1 AX:1 G:1 R:1 AA:2 UW:1 N:1 D:1"),
 ("Old land, new minds, in the one same sound",
  "AO:2 UW:1 L:1 D:1 L:1 AE:3 N:1 D:1 sil:1 N:1 Y:1 UW:3 M:1 AA:2 IY:1 N:1 D:1 Z:1 sil:1 IH:1 N:1 DH:1 AX:1 W:1 AH:2 N:1 S:1 EH:2 IY:1 M:1 S:1 AA:2 UW:1 N:1 D:1"),
 ("Respect to the people, this is native title land",
  "R:1 IH:1 S:1 P:1 EH:2 K:1 T:1 T:1 UW:1 DH:1 AX:1 P:1 IY:2 P:1 AX:1 L:1 sil:1 DH:1 IH:1 S:1 IH:1 Z:1 N:1 EH:1 IY:1 T:1 IH:1 V:1 T:1 AA:1 IY:1 T:1 AX:1 L:1 L:1 AE:3 N:1 D:1"),
 ("Wires in the gum trees, dust on our hands",
  "W:1 AA:2 IY:1 ER:1 Z:1 IH:1 N:1 DH:1 AX:1 G:1 AH:2 M:1 T:1 R:1 IY:2 Z:1 sil:1 D:1 AH:2 S:1 T:1 AA:1 N:1 AA:1 UW:1 ER:1 HH:1 AE:3 N:1 D:1 Z:1"),
]
out = ['# The words, as phonemes on the 16th grid (one slot = one 16th):']
score = []
for words, ph in LINES:
    slots = []
    for tok in ph.split():
        p, n = tok.split(':'); slots += [IDS.index(p)] * int(n)
    assert len(slots) <= 48, (words, len(slots))
    slots += [0] * (48 - len(slots))
    score += slots
    out.append(f'#   {words}')
    out.append(f'#     {ph}')
out.append('let lyr = [')
for i in range(0, len(score), 48):
    out.append('  ' + ', '.join(map(str, score[i:i + 48])) + (',' if i + 48 < len(score) else ''))
out.append(']')
print('\n'.join(out))
