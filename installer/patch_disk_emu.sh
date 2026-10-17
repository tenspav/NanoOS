#!/usr/bin/env bash
exec python3 "$(cd "$(dirname "$0")" && pwd)/patch_disk_emu.py" "$@"
