#!/bin/sh
# Copyright (c) 2026 CallMeLewis
# What the car's sound system looks like, for sound over Wi-Fi: read only, by hand, with the car's BusyBox.
#
#   sh car_sound_probe.sh <label>      writes /tmp/aa-sound-probe-<label>.txt
#
# Run it once per situation, the label saying which: "radio" (radio playing), "bt" (the phone's music playing over
# Bluetooth), "carplay" (CarPlay playing, if there is an iPhone at hand), "nav" (while an iDrive navigation prompt
# speaks), "idle" (sound off). What differs between the files tells which sound output each source uses.
#
# Reads files and lists processes; changes nothing, sends nothing on the car's network, writes only its report in
# /tmp (memory). The report holds no password, VIN or Bluetooth address that this script knows of: read it before
# sending it to anyone all the same.
label=${1:-run}
case $label in
    *[!A-Za-z0-9_-]*)
        echo "label: letters, digits, - and _ only" >&2
        exit 2
        ;;
esac
out=/tmp/aa-sound-probe-$label.txt

section() {
    printf '\n===== %s =====\n' "$1"
}

# A file with its name, when it exists and is readable.
show() {
    if [ -r "$1" ] && [ -f "$1" ]; then
        printf -- '--- %s\n' "$1"
        cat "$1"
        printf '\n'
    fi
}

probe() {
    section "system"
    date
    head -n 1 /etc/version 2>/dev/null
    uname -a
    echo "label: $label"

    section "sound configuration files"
    show /etc/asound.conf
    show /root/.asoundrc
    show "$HOME/.asoundrc"
    for f in /etc/alsa/*.conf /etc/alsa/conf.d/*.conf /usr/share/alsa/alsa.conf.d/*.conf; do
        show "$f"
    done
    ls -l /usr/share/alsa /etc/alsa 2>/dev/null

    section "other files that declare sound outputs (names only)"
    find / \( -path /proc -o -path /sys -o -path /dev -o -path /tmp \) -prune -o -type f \
        \( -name '*.conf' -o -name '*.cfg' -o -name '*.asoundrc' -o -name 'asound*' \) -print 2>/dev/null |
        while read -r f; do
            if grep -qE '^[[:space:]]*(pcm|ctl)\.|type[[:space:]]+(dmix|dsnoop|plug|route|asym|multi|hw)' "$f" 2>/dev/null; then
                ls -l "$f"
            fi
        done

    section "sound cards (/proc/asound)"
    for f in /proc/asound/version /proc/asound/cards /proc/asound/devices /proc/asound/pcm; do
        show "$f"
    done
    ls -l /dev/snd 2>/dev/null

    section "every PCM stream right now: who has it open, at what format"
    for d in /proc/asound/card*/pcm*; do
        [ -d "$d" ] || continue
        show "$d/info"
        for s in "$d"/sub*; do
            [ -d "$s" ] || continue
            for f in status hw_params sw_params; do
                printf -- '--- %s/%s: ' "$s" "$f"
                tr '\n' ' ' <"$s/$f" 2>/dev/null
                printf '\n'
            done
        done
    done

    section "processes holding a sound device or the sound library"
    for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        devices=$(for l in "$p"/fd/*; do
            t=$(readlink "$l" 2>/dev/null)
            case $t in
                /dev/snd* | /dev/shm*) echo "$t" ;;
            esac
        done | sort -u | tr '\n' ' ')
        library=$(grep -c 'libasound' "$p/maps" 2>/dev/null)
        if [ -n "$devices" ] || [ "${library:-0}" -gt 0 ]; then
            printf '%s  %s\n' "$pid" "$(tr '\0' ' ' <"$p/cmdline" 2>/dev/null)"
            [ -n "$devices" ] && printf '      open: %s\n' "$devices"
            [ "${library:-0}" -gt 0 ] && printf '      uses libasound\n'
            tr '\0' '\n' <"$p/environ" 2>/dev/null | grep -i '^ALSA' | sed 's/^/      env: /'
        fi
    done

    section "shared sound buffers (dmix, dsnoop)"
    ls -l /dev/shm 2>/dev/null
    ipcs 2>/dev/null

    section "files named after sound, CarPlay or the sound chip"
    find / \( -path /proc -o -path /sys -o -path /dev -o -path /tmp \) -prune -o \
        \( -iname '*asound*' -o -iname '*alsa*' -o -iname '*audio*' -o -iname '*carplay*' -o -iname '*airplay*' \
        -o -iname '*dirana*' -o -iname '*a2dp*' -o -iname '*aplay*' -o -iname '*amixer*' \) -print 2>/dev/null |
        head -n 400

    section "all processes"
    ps 2>/dev/null || ps -ef

    section "D-Bus names (system bus)"
    if command -v dbus-send >/dev/null 2>&1; then
        dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
            org.freedesktop.DBus.ListNames 2>&1 | grep string
    else
        echo "no dbus-send"
    fi

    section "mounts"
    mount
}

probe >"$out" 2>&1
echo "written: $out ($(wc -c <"$out") bytes). Copy it to the PC with scp, then it goes at the next restart."
