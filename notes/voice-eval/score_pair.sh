#!/bin/sh
# score_pair.sh A B: score two variants in parallel (separate temp names).
cd ~/whisper
run() { v=$1; sed "s#/tmp/vo_in.aither#/tmp/vo_in_$v.aither#; s#vo_in.aither 72 vo_out.wav#vo_in_$v.aither 72 vo_out_$v.wav#; s#/tmp/vo_out.wav#/tmp/vo_out_$v.wav#; s#/tmp/vo.txt#/tmp/vo_$v.txt#" score_voice.sh > score_$v.sh; sh score_$v.sh v$v.aither > res_$v.txt 2>&1; }
run $1 & run $2 & wait
for v in $1 $2; do echo "== $v"; tail -9 res_$v.txt; done
