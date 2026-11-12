#!/bin/sh
# Copyright (c) 2026 CallMeLewis
# Copies the head unit's system to the PC, before anything is installed: a reference to compare with or restore a file
# from, and the copy of the car's system the ARM build and the QEMU tests need (~/entrynav2/rootfs).
#
#   sh tools/backup_headunit.sh [--host 169.254.1.0] [--out DIR] [-- SSH-OPTIONS...]
#   sh tools/backup_headunit.sh --print-remote      shows what runs on the car, and runs nothing
#
# Runs on the PC (Linux, WSL, macOS or Git Bash on Windows) with ssh, over the ENET cable; asks for the root password
# once. On the car it only reads: a few system facts go to /tmp/aa-entrynav-backup (memory), and tar streams the files
# over the SSH session; nothing is written to the flash, nothing is started or stopped. Left out: the pseudo file
# systems (/proc, /sys, /dev...), memory file systems (/tmp, /run...), removable media (/mnt, /media), and
# /var/local, the data partition, which holds personal data (paired phones, settings).
#
# The copy holds BMW files and the car's own settings (its Wi-Fi password among them): it stays on this PC, outside
# the repository, and is never shared.
set -u

HOST=169.254.1.0
OUT=${HOME:-.}/entrynav2/backups

remote_script() {
    cat <<'REMOTE'
# AA-EntryNav backup, on the head unit: reads only. Facts to /tmp (memory), files to stdout as a tar stream.
I=/tmp/aa-entrynav-backup
rm -rf "$I"
mkdir -p "$I" || exit 1
{
    echo "date: $(date)"
    head -n 5 /etc/version
    uname -a
} >"$I/system.txt" 2>&1
cat /proc/mounts >"$I/mounts.txt" 2>&1
df -k >"$I/df.txt" 2>&1
cat /proc/partitions >"$I/partitions.txt" 2>/dev/null
cat /proc/mtd >"$I/mtd.txt" 2>/dev/null
cat /proc/cmdline >"$I/cmdline.txt" 2>/dev/null
{
    busybox 2>&1 | head -n 1
    busybox --list 2>/dev/null
} >"$I/busybox.txt"
ls -la / /opt /opt/hmi/data/ro >"$I/listing.txt" 2>&1

# The top-level folders to copy.
skip=" dev proc sys tmp run mnt media lost+found "
list=
for e in /*; do
    n=${e#/}
    case $skip in
        *" $n "*) continue ;;
    esac
    if [ -e "$e" ] || [ -L "$e" ]; then
        list="$list $n"
    fi
done
# Left out inside them: file systems with nothing of the flash in them, and the data partition.
: >"$I/exclude.txt"
while read -r _dev mnt type _rest; do
    n=${mnt#/}
    case " $list " in
        *" ${n%%/*} "*) ;;
        *) continue ;;
    esac
    case $type in
        proc | sysfs | tmpfs | devtmpfs | devpts | ramfs | cgroup | cgroup2 | debugfs | securityfs | configfs | \
            fusectl | mqueue | hugetlbfs | pstore | rpc_pipefs | binfmt_misc | autofs | tracefs | nfsd)
            echo "$n" >>"$I/exclude.txt"
            ;;
    esac
done </proc/mounts
echo var/local >>"$I/exclude.txt"
echo "$list" >"$I/included.txt"

cd / || exit 1
if tar -cf /dev/null -X "$I/exclude.txt" tmp/aa-entrynav-backup/system.txt 2>/dev/null; then
    # shellcheck disable=SC2086
    tar -cf - -X "$I/exclude.txt" $list tmp/aa-entrynav-backup
    status=$?
else
    # This tar cannot exclude: /var without /var/local, and a warning for the rest.
    echo "warning: this unit's tar cannot leave folders out; mounts inside the copied folders are included" >&2
    echo "no-exclude" >"$I/tar-mode.txt"
    files=
    for n in $list; do
        if [ "$n" = var ]; then
            for v in /var/*; do
                [ "$v" = /var/local ] || files="$files ${v#/}"
            done
        else
            files="$files $n"
        fi
    done
    # shellcheck disable=SC2086
    tar -cf - $files tmp/aa-entrynav-backup
    status=$?
fi
rm -rf "$I"
exit $status
REMOTE
}

usage() {
    cat <<'EOF2'
Usage: sh tools/backup_headunit.sh [--host 169.254.1.0] [--out DIR] [-- SSH-OPTIONS...]
       sh tools/backup_headunit.sh --print-remote      shows what runs on the car, and runs nothing
EOF2
}

while [ $# -gt 0 ]; do
    case $1 in
        --host)
            HOST=${2:?--host needs an address}
            shift 2
            ;;
        --out)
            OUT=${2:?--out needs a folder}
            shift 2
            ;;
        --print-remote)
            remote_script
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

case $OUT in
    /*) ;;
    *) OUT=$(pwd)/$OUT ;;
esac
# Never inside a git repository (this one or any other), checked before anything is created. Looked up by hand: git
# may refuse to answer for a folder another user owns.
dir=$OUT
while :; do
    if [ -e "$dir/.git" ]; then
        echo "The copy holds BMW files: $OUT is inside the git repository $dir; choose another folder (--out)." >&2
        exit 2
    fi
    if [ "$dir" = / ] || [ "$dir" = . ] || [ -z "$dir" ]; then
        break
    fi
    dir=$(dirname "$dir")
done
mkdir -p "$OUT" || exit 1
stamp=$(date +%Y%m%d-%H%M%S)
tarfile=$OUT/headunit-$stamp.tar

echo "Copying the head unit's system from root@$HOST to $tarfile"
echo "Nothing is written to the car's flash. Enter the root password when asked; this takes a few minutes."
remote_script | ssh "$@" "root@$HOST" 'cat >/tmp/aa-entrynav-backup.sh && sh /tmp/aa-entrynav-backup.sh; s=$?; rm -f /tmp/aa-entrynav-backup.sh; exit $s' >"$tarfile"
status=$?
if [ $status -eq 255 ]; then
    echo "FAIL: no SSH session (exit 255); the partial file stays at $tarfile" >&2
    exit 1
fi

listing=$(tar -tf "$tarfile" 2>/dev/null) || {
    echo "FAIL: the copy stopped (exit $status): $tarfile is not a complete tar file" >&2
    exit 1
}
if [ $status -ne 0 ]; then
    # The car's tar also answers non-zero for a file it could not read or a kind it does not store (a socket).
    echo "warning: the car's tar reported problems (exit $status): read its messages above; the copy is checked below" >&2
fi
# The facts are added last: with them and /etc/version the copy ran to its end.
missing=
for expected in etc/version tmp/aa-entrynav-backup/system.txt; do
    printf '%s\n' "$listing" | grep -q "^$expected" || missing="$missing $expected"
done
if [ -n "$missing" ]; then
    echo "FAIL: the copy lacks:$missing" >&2
    exit 1
fi
# What the ARM build needs. Their absence does not make the copy wrong (a unit may link /lib elsewhere), only unusual.
for expected in lib/ld-linux-armhf.so.3 opt/hmi/lib/ usr/local/lib/; do
    printf '%s\n' "$listing" | grep -q "^$expected" ||
        echo "warning: no $expected in the copy: not the layout of the reference car, check before building with it" >&2
done
if printf '%s\n' "$listing" | grep -q '^var/local/'; then
    echo "warning: the copy holds /var/local (personal data): keep it to yourself" >&2
fi

software=$(tar -xOf "$tarfile" tmp/aa-entrynav-backup/system.txt 2>/dev/null | sed -n '2p' | tr -cd 'A-Za-z0-9._-')
final=$OUT/headunit-${software:-unknown}-$stamp.tar
mv "$tarfile" "$final" || final=$tarfile
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$OUT" && sha256sum "$(basename "$final")" >"$(basename "$final").sha256")
fi
size=$(du -k "$final" | cut -f1)
count=$(printf '%s\n' "$listing" | wc -l | tr -d ' ')
echo
echo "Done: $final ($size KiB, $count entries; iDrive software ${software:-unknown})."
echo "Facts about the unit (mounts, partitions, BusyBox applets) are inside, in tmp/aa-entrynav-backup/."
echo "For the ARM build and the QEMU tests, unpack it on Linux or WSL (as root keeps owners and modes):"
echo "  mkdir -p ~/entrynav2/rootfs && tar -xpf '$final' -C ~/entrynav2/rootfs"
