#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <ps5-host>" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
bundle_dir=${WILIWILI_PS5_BUNDLE:-"$root/build-ps5/ps5"}
ps5_host=$1
websrv_port=${PS5_WEBSRV_PORT:-8080}
http_port=${WILIWILI_HTTP_PORT:-18180}
local_host=${WILIWILI_LOCAL_HOST:-}
ftp_port=${WILIWILI_PS5_FTP_PORT:-}
app_args=${WILIWILI_PS5_ARGS:-}
app_dir=/data/homebrew/wiliwili
app_path=$app_dir/wiliwili.elf
external_resources=0
if [[ -d "$bundle_dir/resources" ]]; then
    external_resources=1
fi
if [[ "$external_resources" == 1 && -z "$ftp_port" ]]; then
    echo "external PS5 resources require WILIWILI_PS5_FTP_PORT" >&2
    exit 2
fi


for file in wiliwili.elf osmesa-installer.elf libOSMesa.so.8 ca-bundle.crt icon0.png homebrew.js; do
    if [[ ! -f "$bundle_dir/$file" ]]; then
        echo "missing $bundle_dir/$file; run scripts/ps5/build.sh first" >&2
        exit 1
    fi
done

if [[ -z "$ftp_port" ]]; then
    if [[ -z "$local_host" ]]; then
        local_host=$(ip route get "$ps5_host" 2>/dev/null | sed -n 's/.* src \([^ ]*\).*/\1/p' || true)
    fi
    if [[ -z "$local_host" ]]; then
        echo "set WILIWILI_LOCAL_HOST to an address reachable from the PS5" >&2
        exit 1
    fi

    python3 -m http.server "$http_port" --bind "$local_host" --directory "$bundle_dir" >/dev/null 2>&1 &
    server_pid=$!
    trap 'kill "$server_pid" 2>/dev/null || true' EXIT
    sleep 1

    websrv_url="http://${ps5_host}:${websrv_port}/elfldr"
    curl --fail-with-body --connect-timeout 5 --max-time 300 -sS \
        -F "elf=@$bundle_dir/osmesa-installer.elf;filename=osmesa-installer.elf" \
        -F "args=osmesa-installer.elf http://${local_host}:${http_port}/libOSMesa.so.8 http://${local_host}:${http_port}/wiliwili.elf http://${local_host}:${http_port}/ca-bundle.crt http://${local_host}:${http_port}/icon0.png http://${local_host}:${http_port}/homebrew.js" \
        -F 'pipe=1' \
        "$websrv_url"
else
    curl --fail --connect-timeout 5 --max-time 300 --disable-epsv --ftp-create-dirs \
        --user "${WILIWILI_PS5_FTP_USER:-anonymous:}" \
        --upload-file "$bundle_dir/libOSMesa.so.8" \
        "ftp://${ps5_host}:${ftp_port}$app_dir/lib/libOSMesa.so.8"
    curl --fail --connect-timeout 5 --max-time 300 --disable-epsv --ftp-create-dirs \
        --user "${WILIWILI_PS5_FTP_USER:-anonymous:}" \
        --upload-file "$bundle_dir/ca-bundle.crt" \
        "ftp://${ps5_host}:${ftp_port}$app_dir/ca-bundle.crt"
    curl --fail --connect-timeout 5 --max-time 300 --disable-epsv --ftp-create-dirs \
        --user "${WILIWILI_PS5_FTP_USER:-anonymous:}" \
        --upload-file "$bundle_dir/wiliwili.elf" \
        "ftp://${ps5_host}:${ftp_port}$app_path"
    curl --fail --connect-timeout 5 --max-time 300 --disable-epsv --ftp-create-dirs \
        --user "${WILIWILI_PS5_FTP_USER:-anonymous:}" \
        --upload-file "$bundle_dir/icon0.png" \
        "ftp://${ps5_host}:${ftp_port}$app_dir/sce_sys/icon0.png"
    curl --fail --connect-timeout 5 --max-time 300 --disable-epsv --ftp-create-dirs \
        --user "${WILIWILI_PS5_FTP_USER:-anonymous:}" \
        --upload-file "$bundle_dir/homebrew.js" \
        "ftp://${ps5_host}:${ftp_port}$app_dir/homebrew.js"
    if [[ "$external_resources" == 1 ]]; then
        shopt -s globstar nullglob
        for file in "$bundle_dir/resources"/**/*; do
            [[ -f "$file" ]] || continue
            relative=${file#"$bundle_dir"/}
            curl --fail --connect-timeout 5 --max-time 300 --disable-epsv --ftp-create-dirs \
                --user "${WILIWILI_PS5_FTP_USER:-anonymous:}" \
                --upload-file "$file" \
                "ftp://${ps5_host}:${ftp_port}/data/homebrew/wiliwili/${relative}"
        done
        shopt -u globstar nullglob
        echo "OSMesa, CA bundle, wiliwili payload, launcher metadata and resources uploaded via FTP"
    else
        echo "OSMesa, CA bundle, wiliwili payload and launcher metadata uploaded via FTP"
    fi
fi

if [[ "${WILIWILI_PS5_SKIP_LAUNCH:-0}" != 1 ]]; then
    curl --fail-with-body --connect-timeout 5 --max-time 300 -sS --get \
        --data-urlencode "path=$app_path" \
        --data-urlencode "cwd=$app_dir" \
        --data-urlencode "args=$app_args" \
        "http://${ps5_host}:${websrv_port}/hbldr"
fi

echo "wiliwili deployed"
