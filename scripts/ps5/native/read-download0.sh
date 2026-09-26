#!/usr/bin/env bash
# Read a file out of a title's download data area (download0.dat).
#
#   read-download0.sh <ps5-host> <title-id> <path-inside-download0>
#
# The title's only writable mount is an UFS2 image on the console. Diagnostics
# that must be read back deterministically (UDP logs lose lines and depend on
# when the listener started) are written there and read with this script.
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <ps5-host> <title-id> <path>" >&2
    exit 2
fi

host=$1
title_id=$2
inner=$3
websrv_port=${PS5_WEBSRV_PORT:-8080}

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
tool=${UFS2TOOL:-$repo/build-ps5/pkg-experiment/third_party/ufs2tool/linux-x64/linux-x64/UFS2Tool}
[[ -x "$tool" ]] || { echo "UFS2Tool not found at $tool" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl --fail --connect-timeout 5 --max-time 900 -sS \
    "http://$host:$websrv_port/fs/user/download/$title_id/download0.dat" \
    -o "$work/download0.dat"

DOTNET_ROOT=${DOTNET_ROOT:-/opt/dotnet} PATH="${DOTNET_ROOT:-/opt/dotnet}:$PATH" \
    "$tool" extract "$work/download0.dat" "$work/root" >/dev/null

target="$work/root/$inner"
[[ -f "$target" ]] || { echo "no such file in the image: $inner" >&2; exit 1; }
cat "$target"
