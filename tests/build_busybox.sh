#!/bin/sh
# Copyright (c) 2026 CallMeLewis
# Builds the BusyBox that tests/test_setup.py runs the installer with, and prints its path.
#
#   sh tests/build_busybox.sh [folder]     (default: ~/.cache/aa-entrynav-busybox; built once, then reused)
#
# Static, so that it runs alone inside the test's chroot, and with BusyBox's default settings, like the car's: commands
# found on the PATH win over the built-in ones (the test's fake df, mv, cp and rm must run), and tar can leave folders
# out (-X). Distribution packages differ on both: Debian's busybox-static runs its own commands first, and Debian's
# busybox has no tar -X. Needs curl, bzip2, gcc, make and a static C library (libc6-dev).
set -eu

VERSION=1.36.1
SHA256=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314  # busybox.net's busybox-1.36.1.tar.bz2.sha256
dir=${1:-$HOME/.cache/aa-entrynav-busybox}
bin=$dir/busybox-$VERSION/busybox

if [ ! -x "$bin" ]; then
    mkdir -p "$dir"
    archive=$dir/busybox-$VERSION.tar.bz2
    if [ ! -f "$archive" ]; then
        curl -fsSL -o "$archive.part" "https://busybox.net/downloads/busybox-$VERSION.tar.bz2"
        mv "$archive.part" "$archive"
    fi
    echo "$SHA256  $archive" | sha256sum -c - >/dev/null || {
        echo "build_busybox: $archive does not match its checksum" >&2
        exit 1
    }
    rm -rf "$dir/busybox-$VERSION"
    tar -xjf "$archive" -C "$dir"
    cd "$dir/busybox-$VERSION"
    make -s defconfig >/dev/null
    # Static; tc left out (it does not build against recent kernel headers, and nothing here needs it).
    sed -i -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' -e 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
    make -s -j"$(nproc)" >build.log 2>&1 || {
        tail -n 20 build.log >&2
        exit 1
    }
fi
echo "$bin"
