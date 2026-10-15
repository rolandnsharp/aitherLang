#!/bin/sh
# score_voice.sh patch.aither: render the voice alone (72 s, all 8 lines) and score it.
set -e
cp "$1" /tmp/vo_in.aither
docker run --rm --cpus 4 --user $(id -u):$(id -g) -e HOME=/tmp -e AITHER_AUDIO=null --entrypoint sh -v ~/aither-tools:/tools -v /tmp:/w -w /w aither-stream -c "/tools/towav vo_in.aither 72 vo_out.wav" > /tmp/vo.txt
. ~/whisper/env.sh
~/zara/.venv/bin/python -I ~/whisper/wer.py /tmp/vo_out.wav 1
