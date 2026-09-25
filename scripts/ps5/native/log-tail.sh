#!/usr/bin/env bash
# Print the boot log of the newest run of an installed native title.
#
# The title writes into its download data area, which is a filesystem image the
# console only writes back to disk when the mount is released - the boot log
# helper calls fsync() so the image stays readable while the title runs.
#
# usage: log-tail.sh <ps5-host> [title-id] [lines]
#
# environment:
#   PS5_FTP_PORT     zftpd port (default 2120)
#   PS5_LOG_WORKDIR  scratch directory (default /tmp/wiliwili-log)

set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "usage: $0 <ps5-host> [title-id] [lines]" >&2
    exit 2
fi

host=$1
title_id=${2:-PPSA99010}
lines=${3:-40}
ftp_port=${PS5_FTP_PORT:-2120}
work=${PS5_LOG_WORKDIR:-/tmp/wiliwili-log}
root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
ufs2tool=$root/build-ps5/pkg-experiment/third_party/ufs2tool/linux-x64/linux-x64/UFS2Tool

mkdir -p "$work"
image=$work/download0.dat
out=$work/extracted

echo "==> fetching download data for $title_id" >&2
curl --fail --connect-timeout 5 --max-time 900 -u anonymous: \
    "ftp://$host:$ftp_port/user/download/$title_id/download0.dat" -o "$image"

rm -rf "$out"
DOTNET_ROOT=${DOTNET_ROOT:-/opt/dotnet} PATH=/opt/dotnet:$PATH \
    "$ufs2tool" extract "$image" "$out" >/dev/null 2>&1

log=$out/wiliwili-boot.log
if [[ ! -f $log ]]; then
    echo "no boot log in the download image (the title has not written one yet)" >&2
    exit 1
fi

# A launch request is ignored while another instance owns the display, so the
# newest run is the one after the last build stamp.
start=$(grep -n '^wiliwili: build ' "$log" | tail -1 | cut -d: -f1 || true)
if [[ -z $start ]]; then
    tail -n "$lines" "$log"
else
    sed -n "${start},\$p" "$log" | tail -n "$lines"
fi
