#!/bin/sh
# Cycle the paint scenes: every 50 s the next one becomes scene.paint.
cd "$(dirname "$0")"
while true; do
  for f in scenes/*.paint; do
    cp "$f" web/scene.paint.tmp && mv web/scene.paint.tmp web/scene.paint
    echo "$(date +%T) $f" > rotate.now
    sleep 50
  done
done
