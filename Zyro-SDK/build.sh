#!/usr/bin/env bash
# Convenience wrapper: ./build.sh MyApp.cpp
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python3 "$DIR/tools/pack_zapp.py" "$@"
