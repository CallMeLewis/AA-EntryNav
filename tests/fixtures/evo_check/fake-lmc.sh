#!/bin/sh
# Copyright (c) 2026 CallMeLewis
# A stand-in for LayerManagerControl, for evo-check --scene side by side under QEMU (tests/run_qemu.py). Each question
# gets a fixed answer, and some fail the ways a command can: an exit status, a failure reported with status zero,
# output past the 64 KiB limit, no answer in time, a signal, and a NUL in the output.
case "$*" in
"get scene") printf 'screen 0 (0x0)\n  layer 1000 (0x3e8)\n    surface 101 (0x65)\n' ;;
"get screens") echo "Screen IDs: 0" ;;
"get screen 0") echo "no screen 0" >&2; exit 3 ;;
"get layers") echo "LayerManagerService returned: failed" ;;
"get surfaces") head -c 70000 /dev/zero | tr '\0' 's'; sleep 1 ;;
"get keyboard focus") exec sleep 3 ;;
"get layer 0x100") kill -9 $$ ;;
"get layer 0x300") printf 'layer 768 (0x300)\n\000hidden after a NUL\n' ;;
*) echo "$*: done" ;;
esac
