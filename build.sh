#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sdk="${SIFLI_SDK:-$(cd "$root/../SiFli-SDK" && pwd)}"

if [ ! -d "$sdk" ]; then
    echo "SiFli-SDK not found: $sdk" >&2
    exit 1
fi

export SIFLI_SDK="$sdk"
export RTT_CC=gcc

env_root="${SIFLI_ENV_ROOT:-$HOME/.sifli}"

if [ -z "${RTT_EXEC_PATH:-}" ]; then
    gcc_bin="$(find "$env_root/tools/arm-none-eabi-gcc" -maxdepth 2 -type d -name bin 2>/dev/null | head -n 1 || true)"
    if [ -n "$gcc_bin" ]; then
        export RTT_EXEC_PATH="$gcc_bin"
    fi
fi

if ! command -v scons >/dev/null 2>&1; then
    scons_bin="$(find "$env_root/envs/default" -maxdepth 5 -name scons.exe -path '*/Scripts/*' 2>/dev/null | head -n 1 || true)"

    if [ -z "$scons_bin" ] || [ -z "${RTT_EXEC_PATH:-}" ]; then
        echo "SiFli build environment is not active." >&2
        echo "Run this once in the SDK directory, then re-run this script:" >&2
        echo "  cd \"$sdk\"" >&2
        echo "  . ./export.sh" >&2
        exit 1
    fi

    export PATH="$(dirname "$scons_bin"):$RTT_EXEC_PATH:$PATH"
fi

export PYTHONPATH="$sdk/tools/build;$sdk/tools/build/default"

cd "$root/app/project"
exec scons --board=sf32lb57-p045vr04 --board_search_path="$root/board" -j 8
