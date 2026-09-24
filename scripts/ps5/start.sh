#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <ps5-host>" >&2
    exit 2
fi

ps5_host=$1
websrv_port=${PS5_WEBSRV_PORT:-8080}
app_path=${WILIWILI_PS5_APP_PATH:-/data/homebrew/wiliwili/wiliwili.elf}
app_cwd=${WILIWILI_PS5_CWD:-/data/homebrew/wiliwili}
app_args=${WILIWILI_PS5_ARGS:-}

curl --fail-with-body --connect-timeout 5 --max-time 300 -sS --get \
    --data-urlencode "path=$app_path" \
    --data-urlencode "cwd=$app_cwd" \
    --data-urlencode "args=$app_args" \
    "http://${ps5_host}:${websrv_port}/hbldr"

echo "wiliwili started"
