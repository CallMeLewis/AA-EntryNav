#!/bin/bash
# Copyright (c) 2026 CallMeLewis
# Builds the base image local/ubuntu:12.10 from Ubuntu's archive, for a Docker that no longer pulls ubuntu:12.10 from
# Docker Hub (its image format was removed). Then:
#   docker build --build-arg BASE=local/ubuntu:12.10 -t entrynav2-tc:4.7 toolchain
# A privileged container runs debootstrap; nothing is installed on the PC itself.
set -euo pipefail
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
docker run --rm --privileged -v "$work:/out" debian:12 bash -ec '
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends debootstrap ca-certificates gnupg >/dev/null
    [ -e /usr/share/debootstrap/scripts/quantal ] || ln -s gutsy /usr/share/debootstrap/scripts/quantal
    debootstrap --arch=amd64 --variant=minbase --no-check-gpg quantal /rootfs http://old-releases.ubuntu.com/ubuntu
    rm -rf /rootfs/var/cache/apt/archives/*.deb /rootfs/dev/*
    tar -C /rootfs -cf /out/rootfs.tar .
'
docker import "$work/rootfs.tar" local/ubuntu:12.10
echo "local/ubuntu:12.10 ready"
