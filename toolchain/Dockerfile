# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Build image matching the EntryNav2 head unit: eglibc 2.15, GCC 4.7, armhf (VFPv3/NEON).
# docker build -t entrynav2-tc:4.7 toolchain   (the name toolchain/tc.sh runs)
#
# A Docker that no longer pulls ubuntu:12.10 ("manifest version 2, schema 1 has been removed"): build the same base
# from Ubuntu's archive with toolchain/base.sh, then
# docker build --build-arg BASE=local/ubuntu:12.10 -t entrynav2-tc:4.7 toolchain
ARG BASE=ubuntu:12.10
FROM ${BASE}
# The same package sources whichever base: the release, its updates and its security fixes, from the archive.
RUN echo "deb http://old-releases.ubuntu.com/ubuntu quantal main restricted universe" > /etc/apt/sources.list \
 && echo "deb http://old-releases.ubuntu.com/ubuntu quantal-updates main restricted universe" >> /etc/apt/sources.list \
 && echo "deb http://old-releases.ubuntu.com/ubuntu quantal-security main restricted universe" >> /etc/apt/sources.list \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --force-yes \
      gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf binutils-arm-linux-gnueabihf \
      make pkg-config file \
 && apt-get clean

# armhf development headers (OpenSSL 1.0.1, D-Bus 1.6, ALSA, libdrm/omap, wayland, glib): packages downloaded and
# unpacked into /opt/hdr-armhf, not installed. The libraries actually linked are the HU's own (/sysroot).
RUN dpkg --add-architecture armhf \
 && echo "deb [arch=armhf] http://old-releases.ubuntu.com/ubuntu quantal main universe" > /etc/apt/sources.list.d/armhf.list \
 && sed -i -e 's#^deb http#deb [arch=amd64,i386] http#' /etc/apt/sources.list \
 && apt-get update \
 && mkdir -p /tmp/debs /opt/hdr-armhf && cd /tmp/debs \
 && apt-get download libssl-dev:armhf libdbus-1-dev:armhf libasound2-dev:armhf libdrm-dev:armhf \
      libwayland-dev:armhf libglib2.0-dev:armhf zlib1g-dev:armhf \
 && for d in *.deb; do dpkg -x "$d" /opt/hdr-armhf; done \
 && rm -rf /tmp/debs \
 && ls /opt/hdr-armhf/usr/include
WORKDIR /work
