# aither with the oscilloscope: the stream image plus a 5-channel engine
# (mix L/R, beam X/Y/Z) and a second mount carrying all five as Ogg Opus.
FROM aither-stream:latest
USER root
COPY src /opt/aither
WORKDIR /opt/aither
RUN nim c --mm:arc --threads:on --opt:speed -d:danger --passC:-O2 \
      --passL:"-ltcc -ldl -lasound" --hints:off --warnings:off \
      --out:aither aither.nim
COPY entrypoint.sh icecast.xml /home/aither/
COPY web /home/aither/web
RUN chmod +x /home/aither/entrypoint.sh && chown -R aither /home/aither
USER aither
ENTRYPOINT ["/home/aither/entrypoint.sh"]
