#!/usr/bin/env bash
# Ask the console to launch an installed title and report what happened.
#
# websrv exposes /launch?titleId=<ID>; the shell then spawns the title through
# SceSysCore. The endpoint answers 503 even on success, so the outcome is read
# from the process list instead of the HTTP status.
#
# usage: launch-native.sh <ps5-host> [title-id]
#
# environment:
#   PS5_WEBSRV_PORT        websrv port (default 8080)
#   PSLDMGR_PORT           Payload Manager port (default 8084)
#   WILIWILI_LAUNCH_WAIT   seconds to wait before reporting (default 20)

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <ps5-host> [title-id]" >&2
    exit 2
fi

host=$1
title_id=${2:-PPSA99010}
websrv_port=${PS5_WEBSRV_PORT:-8080}
pldmgr_port=${PSLDMGR_PORT:-8084}
wait_seconds=${WILIWILI_LAUNCH_WAIT:-20}

echo "==> requesting launch of $title_id"
curl --connect-timeout 3 --max-time 20 -sS \
    "http://$host:$websrv_port/launch?titleId=$title_id" -o /dev/null || true

sleep "$wait_seconds"

state=$(curl --fail --connect-timeout 3 --max-time 10 -sS \
    "http://$host:$pldmgr_port/processes_list" |
    jq -r --arg title "$title_id" \
    '.processes[] | select(.name == "eboot.bin" or .name == $title) | "\(.pid) \(.name) \(.memory)"')

if [[ -n $state ]]; then
    echo "running:"
    echo "$state"
else
    echo "no title process found; read the boot log with:"
    echo "  scripts/ps5/native/app-log.sh $host $title_id"
fi
