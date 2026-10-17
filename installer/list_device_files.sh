#!/usr/bin/env bash
exec python3 "$(cd "$(dirname "$0")" && pwd)/list_device_files.py" "$@"
