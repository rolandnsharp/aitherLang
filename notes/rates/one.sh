#!/bin/sh
# one.sh patch: render at three rates, print features
K=${K:-/tmp/rates}   # where renders go; mkdir $K/r first
p=$1; b=$(echo $p | tr / _)
for r in 44100 88200 176400; do
  AITHER_RATE=$r timeout 300 ./aither $p 4.5 </dev/null 2>/dev/null > $K/r/$b.$r.raw || { echo "$p $r FAIL"; rm -f $K/r/$b.$r.raw; exit 0; }
  echo "$p $r $($K/feat $K/r/$b.$r.raw $r)"
  rm -f $K/r/$b.$r.raw
done
