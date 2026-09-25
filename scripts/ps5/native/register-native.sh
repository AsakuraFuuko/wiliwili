#!/usr/bin/env bash
# Register an installed title directory with the console's shell database.
#
# /system_ex/app/<TITLE_ID>/ must already hold eboot.bin and the modules, and
# /user/app/<TITLE_ID>/sce_sys/param.json the user-side metadata. Installation
# alone does not put the title on the home screen; this payload performs the
# registration call a title sandbox cannot (ps5-payload-sdk install_app sample).
#
# usage: register-native.sh <ps5-host> [title-id]
#
# environment:
#   PS5_NATIVE_OUT        build output directory (default <repo>/build-ps5/native)
#   PS5_WEBSRV_PORT       websrv port (default 8080)
#   PS5_PAYLOAD_SDK       payload SDK (default /opt/ps5-payload-sdk)

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <ps5-host> [title-id]" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out=${PS5_NATIVE_OUT:-$root/build-ps5/native}
sdk=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
host=$1
title_id=${2:-PPSA99010}
websrv_port=${PS5_WEBSRV_PORT:-8080}

payload=$out/register-native.elf
"$sdk/bin/prospero-clang" -Wall -O2 "$root/scripts/ps5/native/register_title.c" \
    -lSceAppInstUtil -o "$payload"

echo "==> registering $title_id"
curl --fail-with-body --connect-timeout 5 --max-time 60 -sS \
    -F "elf=@$payload;filename=register-native.elf" \
    -F "args=register-native.elf $title_id" \
    -F 'pipe=1' \
    "http://$host:$websrv_port/elfldr"
echo
