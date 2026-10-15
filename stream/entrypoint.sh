#!/bin/sh
# PulseAudio (null sinks), icecast, the aither engine and the encoder.
# Passwords are made fresh each start and never leave the box.
# ICE_PORT: icecast port (8300). AITHER_SCOPE=1: the engine opens five
# channels (mix L/R, beam X/Y/Z) and a second mount, /scope.ogg, carries
# all five as Ogg FLAC beside the stereo /live.mp3. Lossless, because a
# scope needs DC (a shape parked off-centre, the beam's intensity) and
# every lossy codec here high-passes it away.
export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p "$XDG_RUNTIME_DIR" "$HOME/.config/pulse"
PORT=${ICE_PORT:-8300}
SRCPW=$(head -c 18 /dev/urandom | base64 | tr -dc A-Za-z0-9)
ADMPW=$(head -c 18 /dev/urandom | base64 | tr -dc A-Za-z0-9)
sed -e "s/@SRCPW@/$SRCPW/" -e "s/@ADMPW@/$ADMPW/" -e "s/@PORT@/$PORT/" /home/aither/icecast.xml > /tmp/icecast.xml
# The scope's channels are data, not speakers: never remix them.
printf 'enable-remixing = no\nenable-lfe-remixing = no\n' > "$HOME/.config/pulse/daemon.conf"
if [ "$AITHER_SCOPE" = "1" ]; then
  SINK="module-null-sink sink_name=out format=float32le channels=5 channel_map=front-left,front-right,front-center,rear-left,rear-right"
else
  SINK="module-null-sink sink_name=out"
fi
pulseaudio -n --daemonize=yes --exit-idle-time=-1 \
  --load="module-native-protocol-unix" \
  --load="$SINK" \
  --load="module-null-sink sink_name=silence"
sleep 1
pactl set-default-sink out
pactl set-default-source silence.monitor      # input() hears silence, not itself
nice -n 10 icecast2 -b -c /tmp/icecast.xml
cd /work
nice -n 5 /opt/aither/aither start > /tmp/aither.log 2>&1 &
sleep 2
for p in "$@"; do /opt/aither/aither send "$p" 3; done
ICE="icecast://source:$SRCPW@127.0.0.1:$PORT"
# Only the encoder restarts if it dies; the engine (and the music) keeps going.
while true; do
  if [ "$AITHER_SCOPE" = "1" ]; then
    # parec names all five channels; ffmpeg's own pulse input would
    # leave the two it can't name silent.
    parec -d out.monitor --format=float32le --rate=48000 --channels=5 \
      --channel-map=front-left,front-right,front-center,rear-left,rear-right --latency-msec=20 |
    nice -n 10 ffmpeg -nostdin -loglevel warning -f f32le -ar 48000 -ac 5 -i - \
      -filter_complex "[0:a]pan=stereo|c0=c0|c1=c1[mix]" \
      -map "[mix]" -c:a libmp3lame -b:a 192k -content_type audio/mpeg -f mp3 "$ICE/live.mp3" \
      -map 0:a -c:a flac -sample_fmt s16 -compression_level 5 -frame_size 1024 \
      -page_duration 100000 -flush_packets 1 \
      -content_type application/ogg -f ogg "$ICE/scope.ogg"
  else
    nice -n 10 ffmpeg -nostdin -loglevel warning -f pulse -i out.monitor -ac 2 -ar 48000 \
      -c:a libmp3lame -b:a 192k -content_type audio/mpeg -f mp3 "$ICE/live.mp3"
  fi
  sleep 1
done
