#!/usr/bin/env bash
# Fetch an installed title's boot log from the console.
#
# A title has no visible stdout and the klog service is denied to its sandbox,
# so the application records startup checkpoints in /download0/wiliwili-boot.log.
# That mount is a UFS2 image (download0.dat) on the console; this script copies
# the image over HTTP, extracts the log with UFS2Tool and prints it.
#
# usage: app-log.sh <ps5-host> [title-id]
#
# environment:
#   PS5_WEBSRV_PORT   websrv port (default 8080)
#   UFS2TOOL          UFS2Tool executable (default: the copy fetched by tile tooling)

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <ps5-host> [title-id]" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
host=$1
title_id=${2:-PPSA99010}
websrv_port=${PS5_WEBSRV_PORT:-8080}

tool=${UFS2TOOL:-}
if [[ -z $tool ]]; then
    for candidate in "$root/build-ps5/pkg-experiment/third_party/ufs2tool/linux-x64/linux-x64/UFS2Tool" \
        "$root/build-ps5/tile/tools/UFS2Tool"; do
        [[ -x $candidate ]] && tool=$candidate && break
    done
fi
[[ -n $tool ]] || {
    echo "UFS2Tool not found; set UFS2TOOL to its executable path" >&2
    exit 1
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "==> fetching download data for $title_id"
curl --fail --connect-timeout 5 --max-time 900 -sS \
    "http://$host:$websrv_port/fs/user/download/$title_id/download0.dat" \
    -o "$work/download0.dat"

DOTNET_ROOT=${DOTNET_ROOT:-/opt/dotnet} PATH="${DOTNET_ROOT:-/opt/dotnet}:$PATH" \
    "$tool" extract "$work/download0.dat" "$work/root" >/dev/null

log="$work/root/wiliwili-boot.log"
if [[ ! -f $log ]]; then
    echo "no boot log in the image (the title never reached its first checkpoint)" >&2
    exit 1
fi

tail -n "${WILIWILI_LOG_LINES:-40}" "$log"
