#!/bin/sh
# Copyright (c) 2026 CallMeLewis
# Checks, installs, updates and removes AA-EntryNav on the head unit, with the car's BusyBox 1.21 (docs/INSTALLATION.md).
#
#   sh setup.sh check [--other-software]    every check of the installation; writes nothing
#   sh setup.sh install [--other-software]  the same checks, then the installation, if and only if they all pass
#   sh setup.sh status                      what is installed and in what state; writes nothing
#   sh setup.sh tidy                        deletes what an earlier operation had to leave until a restart
#   sh setup.sh uninstall [--purge]         removes the two folders; --purge also deletes settings and logs
#
# check and install run from the unpacked release archive, in /tmp. The installation puts this script in
# /opt/aa/setup.sh, from where status, tidy and uninstall work without the archive.
#
# How the system partition is written (install, tidy, uninstall):
#   - only once every check has passed; it is read-only before, and goes back to read-only within seconds;
#   - new files are first copied to working folders (/opt/.aa-entrynav-aa.new, /opt/.aa-entrynav-plugin.new), outside
#     iDrive's own folders, and checked against MD5SUMS;
#   - the two folders this project adds (/opt/aa, /opt/hmi/data/ro/Plugins/AAEntryNav) only ever change by renaming a
#     whole folder, which is atomic: whatever happens to the power or to the SSH session, iDrive finds either no app
#     folder, the whole previous version or the whole new one, each with its own programs;
#   - what they are renamed to (/opt/.aa-entrynav-aa.old, /opt/.aa-entrynav-plugin.old) is deleted afterwards, unless a
#     running program still uses it: deleting it would keep the system writable until the restart. Then a later
#     tidy, install or uninstall deletes it, after the restart;
#   - hang-ups and Ctrl-C are ignored while the system is writable, and leaving always puts it back to read-only;
#   - /opt/aa and the app folder are only touched when they carry .installed-by-aa-entrynav; anything else stops.
# Nothing BMW is replaced, and nothing is written to the car's network. Each run is logged to /tmp/aa-entrynav-setup.log.

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
umask 022

AA=/opt/aa
PLUGINS=/opt/hmi/data/ro/Plugins
PLUGIN=$PLUGINS/AAEntryNav
# Working folders: hidden, named for this project alone, in /opt and not in iDrive's data folder (nothing of iDrive
# reads /opt itself), on the same partition as their targets (check_layout).
AA_NEW=/opt/.aa-entrynav-aa.new
AA_OLD=/opt/.aa-entrynav-aa.old
PLUGIN_NEW=/opt/.aa-entrynav-plugin.new
PLUGIN_OLD=/opt/.aa-entrynav-plugin.old
LEFTOVERS="$AA_NEW $AA_OLD $PLUGIN_NEW $PLUGIN_OLD"
DATA=/var/local/data/AA
RECORD=$DATA/install-record
MARK=.installed-by-aa-entrynav
LOG=/tmp/aa-entrynav-setup.log
SELF_COPY=/tmp/aa-entrynav-setup.sh
# /etc/version of the cars this release was tried on (docs/COMPATIBILITY.md).
TRIED_SOFTWARE="202111011321 202306092158"
MARGIN_KB=16384
LIBS=/usr/local/lib:/opt/hmi/lib:/usr/lib:/lib
LOADER=/lib/ld-linux-armhf.so.3
NEEDED_COMMANDS="cat chmod cp df du grep head id ls md5sum mkdir mount mv pidof rm rmdir sync tail touch"
AA_REQUIRED="aa-entrynav evo-trial setup.sh VERSION"
PLUGIN_REQUIRED="libAAEntryNav.so AAEntryNav.manifest AAEntryNav.xml"
EXECUTABLES="aa-entrynav evo-trial libAAEntryNav.so"

# File systems on which a rename survives a power cut whole (journalled or log-structured).
SAFE_FILESYSTEMS="ext3 ext4 ubifs jffs2 yaffs yaffs2 f2fs btrfs xfs"

STOPS=0
WARNS=0
WRITABLE=0
PLUGINS_MADE=0
RECORD_WRITTEN=0
DATA_CREATED=0
NEW_AA_IN=0

# ---- output -----------------------------------------------------------------------------------------------------------
# Every line also goes to the log: the terminal may be gone (SSH dropped) while the system is writable.
say() {
    printf '%s\n' "$*" 2>/dev/null
    printf '%s\n' "$*" >>"$LOG" 2>/dev/null
}
ok() { say "  ok    $*"; }
info() { say "  info  $*"; }
warn() {
    say "  warn  $*"
    WARNS=$((WARNS + 1))
}
stop() {
    say "  STOP  $*"
    STOPS=$((STOPS + 1))
}

usage() {
    cat <<'EOF'
Usage:
  sh setup.sh check [--other-software]    every check of the installation; writes nothing
  sh setup.sh install [--other-software]  the checks, then the installation if they all pass
  sh setup.sh status                      what is installed; writes nothing
  sh setup.sh tidy                        deletes what an earlier operation left until a restart
  sh setup.sh uninstall [--purge]         removes Android Auto; --purge also deletes settings and logs
EOF
}

# ---- reading the system -----------------------------------------------------------------------------------------------
# Helpers name their variables _like_this: shell variables are global, and a helper called inside a loop must not
# change the loop's.

# The mount point holding a path: the longest mount point of /proc/mounts that is a prefix of it.
mount_of() {
    _best=/
    while read -r _dev _mnt _rest; do
        case $1 in
            "$_mnt" | "$_mnt"/*)
                if [ ${#_mnt} -gt ${#_best} ]; then
                    _best=$_mnt
                fi
                ;;
        esac
    done </proc/mounts
    printf '%s\n' "$_best"
}

# The options of the last mount on / (the one in effect).
root_options() {
    _opts=
    while read -r _dev _mnt _type _o _rest; do
        if [ "$_mnt" = / ]; then
            _opts=$_o
        fi
    done </proc/mounts
    printf '%s\n' "$_opts"
}

root_read_only() {
    case ,$(root_options), in
        *,ro,*) return 0 ;;
    esac
    return 1
}

# True when a running program maps or holds open a file under the folder, or stands in it (a shell after "cd"):
# deleting such a file or folder leaves it on the flash until the program lets go, and the system writable with it.
in_use() {
    for _map in /proc/[0-9]*/maps; do
        if [ -r "$_map" ] && grep -qF "$1/" "$_map" 2>/dev/null; then
            return 0
        fi
    done
    for _fds in /proc/[0-9]*/fd; do
        # One ls per process rather than one readlink per open file; the names listed are numbers.
        # shellcheck disable=SC2010
        if [ -d "$_fds" ] && ls -l "$_fds" 2>/dev/null | grep -qF -- "-> $1/"; then
            return 0
        fi
    done
    # shellcheck disable=SC2010
    if ls -ld /proc/[0-9]*/cwd 2>/dev/null | grep -q -e "-> $1/" -e "-> $1\$"; then
        return 0
    fi
    return 1
}

# The folder behind a path, symbolic links resolved; nothing when it is not a folder.
real_dir() {
    (cd "$1" 2>/dev/null && pwd -P)
}

# The type of the file system mounted on / (the last one, in effect).
root_filesystem() {
    _fs=
    while read -r _dev _mnt _type _rest; do
        if [ "$_mnt" = / ]; then
            _fs=$_type
        fi
    done </proc/mounts
    printf '%s\n' "$_fs"
}

# Renames a folder to a name that must be free: never into an existing folder, and checked afterwards.
rename_dir() {
    if [ ! -d "$1" ] || [ -L "$1" ] || [ -e "$2" ] || [ -L "$2" ]; then
        return 1
    fi
    mv "$1" "$2" && [ ! -e "$1" ] && [ -d "$2" ]
}

ours() {
    [ -d "$1" ] && [ -f "$1/$MARK" ]
}

# A value of the installation record (key=value lines on the data partition).
record_value() {
    if [ -f "$RECORD" ]; then
        while IFS='=' read -r _key _value; do
            if [ "$_key" = "$1" ]; then
                printf '%s\n' "$_value"
                return 0
            fi
        done <"$RECORD"
    fi
    return 1
}

# Free space of the system partition in KiB: the "Available" column, counted from the end of df's last line (BusyBox
# puts a long device name on a line of its own).
free_kb() {
    # shellcheck disable=SC2046
    set -- $(df -k / 2>/dev/null | tail -n 1)
    if [ $# -lt 4 ]; then
        return 1
    fi
    eval "printf '%s\\n' \"\${$(($# - 2))}\""
}

android_auto_running() {
    [ -n "$(pidof evo-trial aa-entrynav 2>/dev/null)" ]
}

# BusyBox 1.21's timeout takes -t SECONDS; later ones take SECONDS.
with_timeout() {
    _seconds=$1
    shift
    if busybox timeout -t 1 true >/dev/null 2>&1; then
        busybox timeout -t "$_seconds" "$@"
    elif timeout 1 true >/dev/null 2>&1; then
        timeout "$_seconds" "$@"
    else
        "$@"
    fi
}

# ---- checks -----------------------------------------------------------------------------------------------------------
check_basics() {
    if [ "$(id -u 2>/dev/null)" != 0 ]; then
        stop "run as root (the SSH session of the installation)"
    fi
    missing=
    for c in $NEEDED_COMMANDS; do
        command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
    done
    if [ -n "$missing" ]; then
        stop "commands missing on this unit:$missing"
    fi
    if [ ! -r /proc/mounts ]; then
        stop "/proc/mounts unreadable"
        return
    fi
    if root_read_only; then
        ok "system partition read-only, as it should be"
    else
        stop "the system partition is writable right now ($(root_options)): restart the unit first (reboot)"
    fi
    if android_auto_running; then
        stop "Android Auto is running: stop it from its iDrive menu (Stop) first"
    fi
}

# Every folder this project may touch must be on the partition that "mount -o remount,rw /" makes writable, so that
# renaming never turns into a copy: by its path, and by the folder really behind it when a symbolic link is on the way.
# The folders this project renames must be folders themselves, not links.
check_layout() {
    root_mount=$(mount_of /)
    for p in /opt "$AA" /opt/hmi/data/ro "$PLUGINS" "$PLUGIN" $LEFTOVERS; do
        if [ "$(mount_of "$p")" != "$root_mount" ]; then
            stop "$p is on another partition ($(mount_of "$p")) than the system: unknown layout, nothing done"
        elif [ -d "$p" ]; then
            real=$(real_dir "$p")
            if [ -z "$real" ] || [ "$(mount_of "$real")" != "$root_mount" ]; then
                stop "$p leads to ${real:-an unreadable folder}, on another partition than the system: nothing done"
            fi
        fi
    done
    for p in "$AA" "$PLUGIN" $LEFTOVERS; do
        if [ -L "$p" ]; then
            stop "$p is a symbolic link: left alone, nothing done"
        fi
    done
    if [ ! -d /opt/hmi/data/ro ]; then
        stop "/opt/hmi/data/ro absent: not an EntryNav2 iDrive"
    fi
    if [ -e "$PLUGINS" ] && [ ! -d "$PLUGINS" ]; then
        stop "$PLUGINS exists and is not a folder"
    fi
    filesystem=$(root_filesystem)
    case " $SAFE_FILESYSTEMS " in
        *" $filesystem "*) ok "system partition on $filesystem: a rename survives a power cut whole" ;;
        *)
            warn "system partition on '$filesystem': not known to keep a rename whole through a power cut." \
                "Keep the power on during the few seconds of writing"
            ;;
    esac
}

# The two folders: ours (carrying the marker) or left alone. Working folders still in use need a restart first.
check_existing() {
    for d in "$AA" "$PLUGIN"; do
        if [ -e "$d" ] && ! ours "$d"; then
            stop "$d exists but was not installed by AA-EntryNav: left alone, nothing done"
        fi
    done
    for d in $LEFTOVERS; do
        if [ -e "$d" ]; then
            if [ ! -d "$d" ]; then
                stop "$d is not a folder: left alone, nothing done"
            elif in_use "$d"; then
                stop "$d (an earlier version) is still in use: restart the unit (reboot), then run this again"
            else
                info "$d, left by an earlier operation, will be deleted"
            fi
        fi
    done
}

check_data() {
    if [ -d /var/local/data ] && [ -w /var/local/data ]; then
        ok "data partition writable (/var/local/data: settings, logs, crash guard)"
    else
        stop "/var/local/data is not a writable folder: the app's crash guard could not work"
    fi
}

# ---- the archive ------------------------------------------------------------------------------------------------------
# The files to install, from MD5SUMS: AA_FILES and PLUGIN_FILES, plain names only.
read_payload() {
    AA_FILES=
    PLUGIN_FILES=
    if [ ! -f "$HERE/MD5SUMS" ] || [ ! -d "$HERE/opt" ]; then
        return 1
    fi
    while read -r _sum name; do
        case $name in
            opt/aa/*) base=${name#opt/aa/} ;;
            opt/hmi/data/ro/Plugins/AAEntryNav/*) base=${name#opt/hmi/data/ro/Plugins/AAEntryNav/} ;;
            *) continue ;;
        esac
        case $base in
            "" | .* | *[!A-Za-z0-9._-]*) return 1 ;;
        esac
        case $name in
            opt/aa/*) AA_FILES="$AA_FILES $base" ;;
            *) PLUGIN_FILES="$PLUGIN_FILES $base" ;;
        esac
    done <"$HERE/MD5SUMS"
    for f in $AA_REQUIRED; do
        case " $AA_FILES " in
            *" $f "*) ;;
            *) return 1 ;;
        esac
    done
    for f in $PLUGIN_REQUIRED; do
        case " $PLUGIN_FILES " in
            *" $f "*) ;;
            *) return 1 ;;
        esac
    done
    return 0
}

payload_version() {
    head -n 1 "$HERE/opt/aa/VERSION" 2>/dev/null
}

# MD5SUMS of the installed files only, rewritten for another place: $1 for opt/aa, $2 for the iDrive app folder.
sums_for() {
    while read -r sum name; do
        case $name in
            opt/aa/*) printf '%s  %s/%s\n' "$sum" "$1" "${name#opt/aa/}" ;;
            opt/hmi/data/ro/Plugins/AAEntryNav/*)
                printf '%s  %s/%s\n' "$sum" "$2" "${name#opt/hmi/data/ro/Plugins/AAEntryNav/}"
                ;;
        esac
    done <"$HERE/MD5SUMS"
}

# md5sum -c on the list read from stdin; says which files failed.
verify_sums() {
    _list=/tmp/aa-entrynav-sums.$$
    cat >"$_list"
    _out=$(md5sum -c "$_list" 2>&1)
    _status=$?
    rm -f "$_list"
    if [ "$_status" -ne 0 ]; then
        printf '%s\n' "$_out" | grep -v ': OK$' | while read -r _line; do say "        $_line"; done
        return 1
    fi
    return 0
}

check_archive() {
    if ! read_payload; then
        stop "no complete release here ($HERE): run check and install from the unpacked archive"
        return 1
    fi
    if (cd "$HERE" && md5sum -c MD5SUMS >/dev/null 2>&1); then
        ok "archive intact (MD5SUMS), version $(payload_version)"
    else
        stop "archive damaged: some files differ from MD5SUMS; copy and unpack it again"
        (cd "$HERE" && md5sum -c MD5SUMS 2>&1) | grep -v ': OK$' | while read -r line; do say "        $line"; done
        return 1
    fi
    return 0
}

check_software() {
    software=$(head -n 1 /etc/version 2>/dev/null)
    case " $TRIED_SOFTWARE " in
        *" $software "*)
            ok "iDrive software $software, the one tried"
            return
            ;;
    esac
    if [ -n "$OTHER_SOFTWARE" ]; then
        warn "iDrive software '$software' not tried yet: accepted by --other-software"
    else
        stop "iDrive software '$software' not tried yet. The last two checks (libraries, rehearsal) were not run:" \
            "run 'sh setup.sh check --other-software' for them, and install only if nothing else stops" \
            "(docs/COMPATIBILITY.md; please send a compatibility report)"
    fi
}

check_space() {
    # shellcheck disable=SC2046
    set -- $(du -sk "$HERE/opt" 2>/dev/null)
    need=$1
    avail=$(free_kb)
    case $need in
        "" | *[!0-9]*)
            stop "cannot measure the release ($HERE/opt)"
            return
            ;;
    esac
    case $avail in
        "" | *[!0-9]*)
            stop "cannot read the free space of the system partition (df -k /)"
            return
            ;;
    esac
    # Room for the new files next to the current ones, plus the margin.
    if [ "$avail" -ge $((need + MARGIN_KB)) ]; then
        ok "free space: $avail KiB available, $need KiB needed plus $MARGIN_KB KiB margin"
    else
        stop "not enough free space: $avail KiB available, $((need + MARGIN_KB)) KiB needed"
    fi
}

# The unit's own loader resolves every library and function the programs use, running none of them.
check_libraries() {
    if [ ! -x "$LOADER" ]; then
        stop "$LOADER absent: not an armhf EntryNav2"
        return
    fi
    failed=0
    for f in "$HERE/opt/aa/aa-entrynav" "$HERE/opt/aa/evo-trial" "$HERE/opt/hmi/data/ro/Plugins/AAEntryNav/libAAEntryNav.so"; do
        out=$(LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes LD_LIBRARY_PATH=$LIBS "$LOADER" "$f" 2>&1 |
            grep -E 'not found|undefined symbol|not defined in file')
        if [ -n "$out" ]; then
            failed=1
            stop "${f##*/}: something it needs is missing on this car"
            printf '%s\n' "$out" | while read -r line; do say "        $line"; done
        fi
    done
    if [ "$failed" = 0 ]; then
        ok "every library and function the programs use exists on this car"
    fi
}

# evo-trial --compat rehearses a start, reading only. A run that does not reach its last line ("gps") stops.
check_compat() {
    out=$(cd "$HERE" && LD_LIBRARY_PATH=$LIBS XDG_RUNTIME_DIR=/run/system/0 WAYLAND_DISPLAY=wayland-0 HOME=/tmp \
        with_timeout 60 ./opt/aa/evo-trial --compat 2>&1)
    finished=0
    blocks=0
    saved_ifs=$IFS
    set -f
    IFS='
'
    for line in $out; do
        IFS='|'
        # shellcheck disable=SC2086
        set -- $line
        IFS='
'
        case $1 in
            @ok) ;;
            @block)
                blocks=$((blocks + 1))
                stop "compatibility: $2 ${3:+($3)}"
                ;;
            @warn) warn "compatibility: $2 ${3:+($3)}" ;;
            @info) info "compatibility: $2" ;;
            *) say "        $line" ;;
        esac
        if [ "$2" = gps ]; then
            finished=1
        fi
    done
    IFS=$saved_ifs
    set +f
    if [ "$finished" = 0 ]; then
        stop "the compatibility rehearsal (evo-trial --compat) did not finish"
    elif [ "$blocks" = 0 ]; then
        ok "compatibility rehearsal passed (evo-trial --compat)"
    fi
}

check_guards() {
    if [ -e "$DATA/app.disable" ]; then
        warn "$DATA/app.disable is present: Android Auto stays off until you remove it"
    fi
    if [ -f "$DATA/app-guard" ]; then
        info "the crash guard counted earlier failed starts: the installation resets it for the new version"
    fi
}

run_checks() {
    say "AA-EntryNav setup: checks ($(date 2>/dev/null))"
    check_basics
    check_archive || return
    check_software
    check_layout
    check_existing
    check_data
    check_space
    check_guards
    if [ "$STOPS" = 0 ]; then
        check_libraries
    fi
    if [ "$STOPS" = 0 ]; then
        check_compat
    fi
}

# ---- writing the system partition -------------------------------------------------------------------------------------
begin_write() {
    trap '' HUP INT QUIT TERM PIPE
    trap end_write EXIT
    sync
    if ! mount -o remount,rw / 2>>"$LOG"; then
        say "The system partition could not be made writable: nothing was changed."
        return 1
    fi
    WRITABLE=1
    if root_read_only; then
        say "The system partition is still read-only: nothing was changed."
        return 1
    fi
    return 0
}

end_write() {
    if [ "$WRITABLE" = 1 ]; then
        sync
        if mount -o remount,ro / 2>>"$LOG" && root_read_only; then
            WRITABLE=0
            say "System partition back to read-only."
        else
            say "WARNING: the system partition is still writable. Restart the unit now (reboot): it always starts"
            say "read-only. See $LOG."
        fi
    fi
    trap - EXIT HUP INT QUIT TERM PIPE
}

# Deletes a working folder, or keeps it while a program still uses it (then returns 1).
remove_leftover() {
    if [ ! -e "$1" ]; then
        return 0
    fi
    if in_use "$1"; then
        say "Kept until a restart (still in use): $1"
        return 1
    fi
    rm -rf "$1"
}

remove_leftovers() {
    _kept=0
    for _leftover in $LEFTOVERS; do
        remove_leftover "$_leftover" || _kept=1
    done
    return $_kept
}

# The new version in the working folders, marked first (an interrupted copy is still recognised), then checked.
stage() {
    mkdir "$AA_NEW" "$PLUGIN_NEW" || return 1
    touch "$AA_NEW/$MARK" "$PLUGIN_NEW/$MARK" || return 1
    for f in $AA_FILES; do
        cp "$HERE/opt/aa/$f" "$AA_NEW/$f" || return 1
    done
    for f in $PLUGIN_FILES; do
        cp "$HERE/opt/hmi/data/ro/Plugins/AAEntryNav/$f" "$PLUGIN_NEW/$f" || return 1
    done
    for f in $AA_FILES; do
        chmod 644 "$AA_NEW/$f" || return 1
    done
    for f in $PLUGIN_FILES; do
        chmod 644 "$PLUGIN_NEW/$f" || return 1
    done
    for f in $EXECUTABLES; do
        if [ -f "$AA_NEW/$f" ]; then
            chmod 755 "$AA_NEW/$f" || return 1
        fi
        if [ -f "$PLUGIN_NEW/$f" ]; then
            chmod 755 "$PLUGIN_NEW/$f" || return 1
        fi
    done
    # The checksums of the installed files at their final place, for "status" to read them back later.
    sums_for "$AA" "$PLUGIN" >"$AA_NEW/MD5SUMS" || return 1
    chmod 644 "$AA_NEW/MD5SUMS" || return 1
    sync
    sums_for "$AA_NEW" "$PLUGIN_NEW" | verify_sums
}

# After a failed rename: whatever the switch had moved goes back.
undo_switch() {
    say "Putting the previous state back."
    if [ "$NEW_AA_IN" = 1 ]; then
        rename_dir "$AA" "$AA_NEW"
    fi
    if [ ! -e "$AA" ]; then
        rename_dir "$AA_OLD" "$AA"
    fi
    if [ ! -e "$PLUGIN" ]; then
        rename_dir "$PLUGIN_OLD" "$PLUGIN"
    fi
    remove_leftover "$AA_NEW"
    remove_leftover "$PLUGIN_NEW"
    sync
}

# The record on the data partition, written before the system partition: whether this project created the Plugins
# folder (only then does uninstall remove it), and the version. The previous one is kept until the switch is done.
write_record() {
    created=no
    if [ ! -d "$PLUGINS" ] || [ "$(record_value plugins_folder_created)" = yes ]; then
        created=yes
    fi
    RECORD_WRITTEN=0
    DATA_CREATED=0
    if [ ! -d "$DATA" ]; then
        mkdir -p "$DATA" || return 1
        DATA_CREATED=1
    fi
    rm -f "$RECORD.prev"
    if [ -f "$RECORD" ]; then
        cp "$RECORD" "$RECORD.prev" || return 1
    fi
    {
        printf 'version=%s\n' "$(payload_version)"
        printf 'plugins_folder_created=%s\n' "$created"
        printf 'installed=%s\n' "$(date 2>/dev/null)"
    } >"$RECORD.tmp" && sync && mv "$RECORD.tmp" "$RECORD" || return 1
    RECORD_WRITTEN=1
    sync
}

# An installation given up before the switch: the record goes back to what it was (only if this run replaced it),
# and a Plugins folder or data folder this run made goes if still empty.
abandon() {
    if [ "$PLUGINS_MADE" = 1 ]; then
        rmdir "$PLUGINS" 2>/dev/null
    fi
    rm -f "$RECORD.tmp"
    if [ "$RECORD_WRITTEN" = 1 ]; then
        if [ -f "$RECORD.prev" ]; then
            mv "$RECORD.prev" "$RECORD"
        else
            rm -f "$RECORD"
        fi
    fi
    rm -f "$RECORD.prev"
    if [ "$DATA_CREATED" = 1 ]; then
        rmdir "$DATA" 2>/dev/null
    fi
    sync
}

do_install() {
    : >"$LOG"
    run_checks
    say ""
    if [ "$STOPS" != 0 ]; then
        say "Not installed: $STOPS check(s) failed. Nothing was written."
        return 1
    fi
    if ! write_record; then
        abandon
        say "Not installed: cannot write $RECORD. Nothing was written to the system partition."
        return 1
    fi
    say "Installing version $(payload_version). Do not switch the car off for the next few seconds."
    if ! begin_write; then
        abandon
        return 1
    fi
    if ! remove_leftovers; then
        abandon
        say "Not installed: an earlier version is still in use. Restart the unit (reboot), then run this again."
        return 1
    fi
    if ! stage; then
        abandon
        say "Not installed: the copy on the unit failed its check. The installed version is unchanged."
        remove_leftover "$AA_NEW"
        remove_leftover "$PLUGIN_NEW"
        return 1
    fi
    # Android Auto can start by itself 30 s after iDrive: not while its files change.
    if android_auto_running; then
        abandon
        remove_leftover "$AA_NEW"
        remove_leftover "$PLUGIN_NEW"
        say "Not installed: Android Auto started meanwhile. Stop it from its iDrive menu, then run this again."
        return 1
    fi
    if [ ! -d "$PLUGINS" ]; then
        if ! mkdir "$PLUGINS"; then
            abandon
            say "Not installed: cannot create $PLUGINS. The installed version is unchanged."
            return 1
        fi
        PLUGINS_MADE=1
    fi
    # The switch: iDrive app out, programs in, iDrive app in. Stopped anywhere, it leaves either no app folder (iDrive
    # starts without Android Auto) or a complete one next to its own programs.
    if { [ ! -e "$PLUGIN" ] || rename_dir "$PLUGIN" "$PLUGIN_OLD"; } &&
        { [ ! -e "$AA" ] || rename_dir "$AA" "$AA_OLD"; } &&
        rename_dir "$AA_NEW" "$AA" && NEW_AA_IN=1 &&
        rename_dir "$PLUGIN_NEW" "$PLUGIN"; then
        sync
    else
        undo_switch
        abandon
        say "Not installed: a rename failed. The previous state is back."
        return 1
    fi
    rm -f "$RECORD.prev"
    rm -f "$DATA/app-guard" # a new version gets its own three tries
    remove_leftover "$AA_OLD"
    if ! remove_leftover "$PLUGIN_OLD"; then
        say "The previous iDrive app stays in $PLUGIN_OLD until the restart; 'sh $AA/setup.sh tidy' deletes it then."
    fi
    end_write
    say ""
    if ! sums_for "$AA" "$PLUGIN" | verify_sums; then
        say "WARNING: the installed files do not match MD5SUMS. Do not restart: run 'sh $AA/setup.sh uninstall'."
        return 1
    fi
    say "Installed and checked: version $(payload_version)."
    if [ "$WRITABLE" = 1 ]; then
        return 1
    fi
    say "Restart the unit to load it: reboot"
    say "Then 'sh $AA/setup.sh status' reads the files back from the flash, and on the iDrive: Media, Android Auto, Start."
    return 0
}

do_check() {
    : >"$LOG"
    run_checks
    say ""
    if [ "$STOPS" != 0 ]; then
        say "$STOPS check(s) failed: do not install. Nothing was written."
        return 1
    fi
    say "Every check passed ($WARNS warning(s)). Nothing was written."
    say "Before installing: car parked, ignition on and engine off, ideally with a battery charger connected."
    say "To install: sh setup.sh install${OTHER_SOFTWARE:+ --other-software}"
    return 0
}

do_status() {
    say "AA-EntryNav status"
    say "  iDrive software:  $(head -n 1 /etc/version 2>/dev/null)"
    say "  system partition: $(root_options)"
    if ours "$AA"; then
        say "  installed:        version $(head -n 1 "$AA/VERSION" 2>/dev/null) ($(record_value installed))"
    elif [ -e "$AA" ]; then
        say "  $AA exists but was not installed by AA-EntryNav"
    else
        say "  installed:        no"
    fi
    if [ -e "$PLUGIN" ] && ! ours "$PLUGIN"; then
        say "  $PLUGIN exists but was not installed by AA-EntryNav"
    elif [ -e "$PLUGIN" ]; then
        say "  iDrive app:       $PLUGIN"
    elif ours "$AA"; then
        say "  iDrive app:       absent: iDrive does not load Android Auto (finish with: sh $AA/setup.sh install or uninstall)"
    fi
    if ours "$AA" && ours "$PLUGIN" && [ -f "$AA/MD5SUMS" ]; then
        if md5sum -c "$AA/MD5SUMS" >/dev/null 2>&1; then
            say "  files:            all match the checksums of the installed version"
        else
            say "  files:            DIFFER from the installed version (install it again, or uninstall):"
            md5sum -c "$AA/MD5SUMS" 2>&1 | grep -v ': OK$' | while read -r line; do say "                    $line"; done
        fi
    fi
    for d in $LEFTOVERS; do
        if [ -e "$d" ]; then
            if in_use "$d"; then
                say "  left over:        $d (in use until the restart)"
            else
                say "  left over:        $d (sh $AA/setup.sh tidy deletes it)"
            fi
        fi
    done
    for p in $(pidof HmiMain3D 2>/dev/null); do
        if grep -q libAAEntryNav "/proc/$p/maps" 2>/dev/null; then
            say "  iDrive:           running, Android Auto app loaded"
        else
            say "  iDrive:           running, Android Auto app not loaded"
        fi
    done
    if android_auto_running; then
        say "  Android Auto:     running"
    else
        say "  Android Auto:     stopped"
    fi
    if [ -e "$DATA/app.disable" ]; then
        say "  app.disable:      present (the app stays off)"
    fi
    if [ -f "$DATA/app-guard" ]; then
        say "  crash guard:      $(grep -c . "$DATA/app-guard" 2>/dev/null) recent load(s) counted (3 turn the app off)"
    fi
    return 0
}

do_tidy() {
    : >"$LOG"
    check_basics
    check_layout
    present=0
    for d in $LEFTOVERS; do
        if [ -e "$d" ]; then
            present=1
            [ -d "$d" ] || stop "$d is not a folder: left alone"
        fi
    done
    if [ "$STOPS" != 0 ]; then
        say "Nothing done."
        return 1
    fi
    if [ "$present" = 0 ]; then
        say "Nothing to tidy."
        return 0
    fi
    begin_write || return 1
    remove_leftovers
    kept=$?
    end_write
    if [ "$kept" != 0 ]; then
        say "Some folders are still in use: restart the unit (reboot), then run this again."
        return 1
    fi
    say "Tidied."
    return 0
}

do_uninstall() {
    : >"$LOG"
    check_basics
    check_layout
    check_existing
    if [ "$STOPS" != 0 ]; then
        say "Not uninstalled: $STOPS check(s) failed. Nothing was written."
        return 1
    fi
    present=0
    for d in "$AA" "$PLUGIN" $LEFTOVERS; do
        if [ -e "$d" ]; then
            present=1
        fi
    done
    if [ "$present" = 1 ]; then
        say "Removing AA-EntryNav. Do not switch the car off for the next few seconds."
        begin_write || return 1
        remove_leftovers
        # The iDrive app first: once its folder has left Plugins, iDrive no longer loads it.
        if [ -e "$PLUGIN" ] && ! rename_dir "$PLUGIN" "$PLUGIN_OLD"; then
            say "Not uninstalled: $PLUGIN could not be moved. Nothing else was changed."
            return 1
        fi
        pending=0
        remove_leftover "$PLUGIN_OLD" || pending=1
        # While iDrive still holds its app, /opt/aa stays as it is: nothing starts it any more, and its setup.sh
        # finishes the job after the restart.
        if [ "$pending" = 0 ] && [ -e "$AA" ]; then
            if rename_dir "$AA" "$AA_OLD"; then
                remove_leftover "$AA_OLD" || pending=1
            else
                pending=1
            fi
        fi
        if [ "$(record_value plugins_folder_created)" = yes ] && [ -d "$PLUGINS" ]; then
            rmdir "$PLUGINS" 2>/dev/null
        fi
        end_write
        if [ "$pending" = 1 ]; then
            say ""
            say "iDrive no longer loads Android Auto from the next restart. To finish: restart the unit (reboot), then run"
            say "  sh $AA/setup.sh uninstall${PURGE:+ --purge}"
            say "to delete the last files."
            return 0
        fi
    else
        say "AA-EntryNav is not installed on the system partition."
    fi
    rm -f "$RECORD"
    if [ -n "$PURGE" ]; then
        rm -rf "$DATA"
        say "Settings and logs deleted ($DATA)."
    else
        say "Settings kept in $DATA ('sh setup.sh uninstall --purge' deletes them)."
    fi
    say "AA-EntryNav is uninstalled. Restart the unit: reboot"
    return 0
}

# ---- main -------------------------------------------------------------------------------------------------------------
case $0 in
    */*) HERE=${0%/*} ;;
    *) HERE=. ;;
esac
HERE=$(cd "$HERE" && pwd)
SELF=$HERE/${0##*/}

command=$1
if [ $# -gt 0 ]; then
    shift
fi
OTHER_SOFTWARE=
PURGE=
for arg in "$@"; do
    case $command:$arg in
        check:--other-software | install:--other-software) OTHER_SOFTWARE=1 ;;
        uninstall:--purge) PURGE=1 ;;
        *)
            usage
            exit 2
            ;;
    esac
done

case $command in
    check) do_check ;;
    install) do_install ;;
    status) do_status ;;
    tidy | uninstall)
        # Never run from a folder this script deletes: an open deleted file would keep the system writable.
        case $SELF in
            "$AA"/* | "$AA_OLD"/* | "$AA_NEW"/*)
                if cp "$SELF" "$SELF_COPY"; then
                    exec sh "$SELF_COPY" "$command" "$@"
                fi
                say "Cannot copy this script to $SELF_COPY: nothing done."
                exit 1
                ;;
        esac
        # Nor standing in one: this script's own folder would count as in use.
        cd / || exit 1
        if [ "$command" = tidy ]; then
            do_tidy
        else
            do_uninstall
        fi
        ;;
    *)
        usage
        exit 2
        ;;
esac
exit $?
