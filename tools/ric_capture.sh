#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Records the instrument cluster's guidance calls on the head unit during a factory navigation route, from a laptop
# (macOS or Linux) on the ENET cable. It runs evo-ric on the head unit (tools/evo_ric.cpp): a passive listener that
# sends nothing to BMW's programs and writes to /var/local/data/AA/ric, which a head unit restart doesn't wipe.
#
#   sh ric_capture.sh check            reach the head unit, copy evo-ric over, list the bus's names (saved here)
#   sh ric_capture.sh start [minutes]  start recording, detached, for 60 minutes by default (at most 240)
#   sh ric_capture.sh status           still running? uptime (a restart shows as a small one), what was recorded
#   sh ric_capture.sh fetch            copy every recording to ./ric-capture-<date>/, leaving them on the car
#   sh ric_capture.sh stop             stop recording; what was recorded stays
#   sh ric_capture.sh clean            delete the recordings from the car, once fetched
#
# Environment: HU (default root@169.254.1.0), EVO_RIC (the ARM build of evo-ric, default ./evo-ric beside this
# script, then the repository's root). SSH asks for the password once; the connection is reused for 15 minutes.
set -eu

HU=${HU:-root@169.254.1.0}
HERE=$(cd "$(dirname "$0")" && pwd)
if [ -n "${EVO_RIC:-}" ]; then
    BIN=$EVO_RIC
elif [ -f "$HERE/evo-ric" ]; then
    BIN=$HERE/evo-ric
else
    BIN=$HERE/../evo-ric
fi
OUT=/var/local/data/AA/ric
LIBS=/usr/local/lib:/usr/lib:/lib

hu() {
    ssh -o ControlMaster=auto -o ControlPath=/tmp/evo-ric-%C -o ControlPersist=15m -o ConnectTimeout=10 \
        -o ServerAliveInterval=5 -o ServerAliveCountMax=4 "$HU" "$@"
}

push() {
    if [ ! -f "$BIN" ]; then
        echo "no evo-ric at $BIN: build it with 'make dev-tools', or set EVO_RIC" >&2
        exit 1
    fi
    # Through ssh rather than scp: newer scp needs an SFTP server, which the head unit may not have.
    hu 'cat > /tmp/evo-ric.new && chmod 700 /tmp/evo-ric.new && mv /tmp/evo-ric.new /tmp/evo-ric' < "$BIN"
    echo "evo-ric copied to the head unit's /tmp"
}

# /tmp is gone after a head unit restart: the tool is copied again when it's missing.
ensure() {
    if ! hu 'test -x /tmp/evo-ric'; then
        push
    fi
}

cmd=${1:-}
case $cmd in
check)
    push
    stamp=$(date +%Y%m%d-%H%M%S)
    hu "LD_LIBRARY_PATH=$LIBS /tmp/evo-ric --names" > "names-$stamp.txt"
    echo "bus names saved in names-$stamp.txt:"
    grep -E 'RueKoIC|Kombi|NBTEvo|navigation|Navi' "names-$stamp.txt" || echo "(none of the expected names)"
    ;;
start)
    minutes=${2:-60}
    ensure
    hu "if pidof evo-ric > /dev/null; then echo 'already recording'; else
            mkdir -p $OUT && cd / && LD_LIBRARY_PATH=$LIBS nohup /tmp/evo-ric --watch $minutes --out $OUT \
                > /tmp/evo-ric.out 2>&1 < /dev/null &
            sleep 1; pidof evo-ric > /dev/null && echo 'recording for $minutes min' || cat /tmp/evo-ric.out; fi"
    ;;
status)
    hu "echo \"uptime \$(cut -d' ' -f1 /proc/uptime) s, boot \$(cut -c1-8 /proc/sys/kernel/random/boot_id)\";
        if pidof evo-ric > /dev/null; then echo 'recording'; else echo 'not recording'; fi;
        ls -l $OUT 2>/dev/null; f=\$(ls -t $OUT/*.log 2>/dev/null | head -1);
        if [ -n \"\$f\" ]; then echo \"last lines of \$f:\"; tail -n 3 \"\$f\" | cut -c1-200; fi"
    ;;
fetch)
    dir=ric-capture-$(date +%Y%m%d-%H%M%S)
    mkdir -p "$dir"
    files=$(hu "ls $OUT/*.log 2>/dev/null" || true)
    if [ -z "$files" ]; then
        echo "no recordings on the head unit"
        exit 0
    fi
    for f in $files; do
        hu "cat '$f'" > "$dir/$(basename "$f")"
    done
    echo "copied to $dir:"
    ls -l "$dir"
    ;;
stop)
    # shellcheck disable=SC2016  # expanded on the head unit
    hu 'pid=$(pidof evo-ric); if [ -n "$pid" ]; then kill $pid; sleep 1; echo stopped; else echo "not recording"; fi'
    ;;
clean)
    hu "rm -f $OUT/*.log && rmdir $OUT 2>/dev/null; echo 'recordings deleted from the head unit'"
    ;;
*)
    sed -n '3,15p' "$0"
    exit 2
    ;;
esac
