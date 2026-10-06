#!/usr/bin/env bash
# One test cycle for the formal native title: stop the running title, wait for
# its persistent download data and mount to settle, atomically replace the
# package, then capture the boot log.  The package is deliberately refreshed
# under the same title ID; changing the ID would create a new download0/login
# namespace.
#
#   test-cycle.sh [PPSA99233] [listen-seconds] [options-file]
#
# ShadowMountPlus may keep an image mounted while its title is running.  A
# completed upload to a temporary FTP name followed by RNFR/RNTO replacement
# gives the scanner a complete package without touching download0.
set -euo pipefail

title_id=${1:-PPSA99233}
listen_seconds=${2:-90}
options_file=${3:-}
[[ "$title_id" == PPSA99233 ]] || {
    echo "only the formal title PPSA99233 is supported" >&2
    exit 2
}
host=${WILIWILI_PS5_HOST:-192.168.102.118}
ftp_port=${WILIWILI_PS5_FTP_PORT:-2120}
web_port=${WILIWILI_PS5_WEB_PORT:-8080}
mgr_port=${WILIWILI_PS5_MGR_PORT:-8084}
udp_port=${WILIWILI_LOG_UDP_PORT:-9999}
settle_seconds=${WILIWILI_SHADOWMOUNT_SETTLE_SECONDS:-15}
build_marker=${WILIWILI_BUILD_MARKER:-}

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
dist="$repo/build-ps5/native/dist/$title_id"
log="/tmp/$title_id-boot.log"
image="/tmp/$title_id.ffpkg"
remote_dir=/data/homebrew
remote_name="$title_id.ffpkg"
remote_tmp=".${title_id}.ffpkg.upload.$$"
ufs2="$repo/build-ps5/pkg-experiment/third_party/ufs2tool/linux-x64/linux-x64/UFS2Tool"

[[ -f "$dist/eboot.bin" ]] || { echo "no eboot.bin under $dist" >&2; exit 1; }
[[ -x "$ufs2" ]] || { echo "missing UFS2Tool at $ufs2" >&2; exit 1; }

# Prefer the marker embedded in this exact eboot so a stale mounted image is
# distinguishable from this package; callers may override it explicitly.
if [[ -z "$build_marker" ]]; then
    build_marker=$(strings "$dist/eboot.bin" 2>/dev/null |
        grep -m1 -E '^wiliwili: build [A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}$' || true)
fi

# Write the options file here, immediately before packaging: `build-native.sh`
# wipes dist/assets, so writing it after a build is the only order that works.
# Credentials are title-owned state, not package options.  Refuse sensitive
# keys rather than copying them (or merely redacting their console output).
if [[ -n "$options_file" ]]; then
    [[ -f "$options_file" ]] || { echo "no such options file: $options_file" >&2; exit 1; }
    if grep -Eiq '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*(cookie|password|passwd|token|secret|authorization|credential|api[_-]?key)[A-Za-z0-9_]*[[:space:]]*=' "$options_file"; then
        echo "options contains a credential key; keep cookie only in /download0/wiliwili/config/wiliwili_config.json" >&2
        exit 2
    fi
    cp "$options_file" "$dist/assets/wiliwili-options.txt"
    option_keys=$(awk '
        /^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
        {
            key = $0
            if (index(key, "=") > 0) sub(/[[:space:]]*=.*/, "", key)
            else key = "<invalid>"
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
            if (key !~ /^[A-Za-z_][A-Za-z0-9_]*$/) key = "<invalid>"
            printf "%s ", key
        }
    ' "$options_file")
    echo "==> options keys: ${option_keys:-<none>}"
else
    rm -f "$dist/assets/wiliwili-options.txt"
fi
echo "==> cookie source: /download0/wiliwili/config/wiliwili_config.json (not packaged)"

process_pids() {
    local listing
    listing=$(curl -sS --fail -m 10 "http://$host:$mgr_port/processes_list") || return 1
    python3 -c 'import json,sys
try:
    data=json.load(sys.stdin)
except Exception:
    raise SystemExit(1)
for p in data.get("processes", []):
    if p.get("name", "").startswith(("eboot", "PPSA")):
        print(p["pid"])' <<<"$listing"
}

echo "==> stopping any running title (it owns VideoOut and the audio device)"
pids=$(process_pids) || { echo "cannot read title process state" >&2; exit 1; }
for pid in $pids; do
    echo "    kill $pid"
    curl -sS -m 15 "http://$host:$mgr_port/process_kill?pid=$pid" >/dev/null 2>&1 || true
done

remaining=
for wait_round in {1..30}; do
    remaining=$(process_pids) || { echo "cannot verify title shutdown" >&2; exit 1; }
    [[ -z "$remaining" ]] && break
    sleep 2
done
[[ -z "$remaining" ]] || { echo "title process did not exit; refusing to replace mounted image" >&2; exit 1; }
echo "==> title stopped; waiting ${settle_seconds}s for download0 persistence and mount release"
sleep "$settle_seconds"

echo "==> packaging $title_id"
rm -f "$image"
DOTNET_ROOT=/opt/dotnet PATH=/opt/dotnet:$PATH \
    "$ufs2" newfs -D "$dist" "$image" wiliwili >/dev/null

echo "==> uploading $(du -h "$image" | cut -f1) to temporary FTP name"
curl -sS --fail --connect-timeout 5 --max-time 600 --disable-epsv --user anonymous: \
    --upload-file "$image" "ftp://$host:$ftp_port$remote_dir/$remote_tmp"

# The scanner only sees the completed final pathname.  RNFR/RNTO is one FTP
# server-side rename, so it never observes a partially uploaded final image.
echo "==> atomically replacing $remote_dir/$remote_name"
curl -sS --fail --connect-timeout 5 --max-time 60 --disable-epsv --user anonymous: \
    --quote "RNFR $remote_dir/$remote_tmp" --quote "RNTO $remote_dir/$remote_name" \
    "ftp://$host:$ftp_port/"

echo "==> waiting for ShadowMountPlus to register the refreshed image"
sleep 45

( timeout "$listen_seconds" python3 "$repo/scripts/ps5/native/log-listen.py" "$udp_port" |
    sed -uE \
        -e 's/(WILIWILI_TEST_BILI_COOKIE=)[^[:space:]]*/\1<redacted>/g' \
        -e 's/((cookie|authorization|password|passwd|token|secret|api[_-]?key)[=:][[:space:]]*)[^[:space:]]*/\1<redacted>/gi' \
        > "$log" 2>&1 & )
sleep 2

echo "==> launching"
launched=0
for attempt in 1 2 3 4 5 6; do
    launch_status=$(curl -sS --connect-timeout 3 --max-time 25 -o /dev/null -w '%{http_code}' \
        "http://$host:$web_port/launch?titleId=$title_id" || true)
    if [[ "$launch_status" =~ ^2[0-9][0-9]$ ]]; then
        launched=1
        break
    fi
    if pids=$(process_pids) && [[ -n "$pids" ]]; then
        echo "    launch HTTP $launch_status; title process observed only to collect boot marker (not acceptance evidence)"
        launched=1
        break
    fi
    echo "    launch HTTP $launch_status not ready (attempt $attempt), retrying in 15s"
done
[[ $launched == 1 ]] || { echo "launch did not return a 2xx response" >&2; exit 1; }

if (( listen_seconds > 5 )); then
    sleep $((listen_seconds - 5))
else
    sleep 1
fi
if [[ -n "$build_marker" ]]; then
    grep -Fq -- "$build_marker" "$log" || {
        echo "boot log did not contain the requested build marker" >&2
        exit 1
    }
else
    grep -Eq 'wiliwili: build [^[:space:]]+' "$log" || {
        echo "boot log did not contain a wiliwili build marker" >&2
        exit 1
    }
fi
echo "==> boot build marker verified"

echo "==> log ($log)"
cat "$log"
