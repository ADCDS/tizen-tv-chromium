# Base image for the kiosk: Debian trixie armhf with Chromium and Xvfb, slimmed
# (no docs, only the en-US and pt-BR Chromium locales). Built by build-image.sh.
FROM debian:trixie-slim
RUN printf '%s\n' 'path-exclude /usr/share/doc/*' 'path-exclude /usr/share/man/*' 'path-exclude /usr/share/info/*' \
      'path-exclude /usr/share/locale/*' 'path-include /usr/share/locale/locale.alias' > /etc/dpkg/dpkg.cfg.d/01-slim \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      chromium xvfb fonts-dejavu-core fonts-liberation2 ca-certificates pulseaudio-utils \
 && find /usr/lib/chromium/locales -type f ! -name 'en-US.pak' ! -name 'pt-BR.pak' -delete \
 && apt-get clean && rm -rf /var/lib/apt/lists/* /var/cache/apt/* /var/log/*
