# Copyright (c) 2026 CallMeLewis
# The image CI's job runs in (.github/workflows/ci.yml): Debian 12 with the compilers, the style tools and the BusyBox
# the installer test needs, so that a run installs and builds none of them. .github/workflows/ci-image.yml builds it
# and pushes it to ghcr.io/callmelewis/aa-entrynav-ci.
#
# After a change here: raise TAG in ci-image.yml, push, wait for that workflow, then use the new tag in ci.yml.
# Built from the repository root:  docker build -f .github/ci/Dockerfile .
FROM debian:12

RUN apt-get update \
    && apt-get install -y --no-install-recommends bzip2 ca-certificates curl dbus g++ gcc git libdbus-1-dev \
        libssl-dev make pkg-config protobuf-compiler python3 python3-protobuf python3-venv shellcheck \
    && rm -rf /var/lib/apt/lists/* \
    && python3 -m venv /opt/style \
    && /opt/style/bin/pip install --no-cache-dir --quiet clang-format==18.1.8 ruff==0.6.9

# The installer test's BusyBox (tests/build_busybox.sh), at /opt/busybox/busybox-<version>/busybox. Only the program
# is kept.
COPY tests/build_busybox.sh /tmp/build_busybox.sh
RUN bin=$(sh /tmp/build_busybox.sh /opt/busybox) \
    && mv "$bin" /opt/busybox/busybox.keep \
    && rm -rf /opt/busybox/busybox-* /tmp/build_busybox.sh \
    && mv /opt/busybox/busybox.keep /opt/busybox/busybox \
    && /opt/busybox/busybox true
