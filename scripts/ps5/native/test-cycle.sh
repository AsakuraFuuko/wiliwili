#!/usr/bin/env bash
# One test cycle for the native title: stop the running title, delete every old
# test image, upload the freshly built one and capture its UDP boot log.
#
#   test-cycle.sh <title-id> [listen-seconds]
#
# A new TITLE_ID is required for every cycle: ShadowMountPlus caches a title's
# assets per id and silently keeps serving the old copy ("[SPEED] Skipping file
# copy (Assets already exist)"), so a rebuilt image under the same id can run
# stale options/assets. The running title must also exit first, because it owns
# VideoOut and the audio device.
set -euo pipefail

title_id=${1:?usage: test-cycle.sh <title-id> [listen-seconds] [options-file]}
listen_seconds=${2:-90}
options_file=${3:-}
host=${WILIWILI_PS5_HOST:-192.168.102.118}
ftp_port=${WILIWILI_PS5_FTP_PORT:-2120}
web_port=${WILIWILI_PS5_WEB_PORT:-8080}
mgr_port=${WILIWILI_PS5_MGR_PORT:-8084}
udp_port=${WILIWILI_LOG_UDP_PORT:-9999}

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
dist="$repo/build-ps5/native/dist/$title_id"
image="/tmp/$title_id.ffpkg"
log="/tmp/$title_id-boot.log"
ufs2="$repo/build-ps5/pkg-experiment/third_party/ufs2tool/linux-x64/linux-x64/UFS2Tool"

[[ -f "$dist/eboot.bin" ]] || { echo "no eboot.bin under $dist" >&2; exit 1; }
[[ -x "$ufs2" ]] || { echo "missing UFS2Tool at $ufs2" >&2; exit 1; }

# Write the options file here, immediately before packaging: `build-native.sh`
# wipes dist/assets, so writing it after a build is the only order that works.
if [[ -n "$options_file" ]]; then
    [[ -f "$options_file" ]] || { echo "no such options file: $options_file" >&2; exit 1; }
    cp "$options_file" "$dist/assets/wiliwili-options.txt"
    safe_options=$(sed 's/^WILIWILI_TEST_BILI_COOKIE=.*/WILIWILI_TEST_BILI_COOKIE=<redacted>/' "$options_file" | tr '\n' ' ')
    echo "==> options: $safe_options"
else
    rm -f "$dist/assets/wiliwili-options.txt"
fi

echo "==> stopping any running title (it owns VideoOut and the audio device)"
for pid in $(curl -sS -m 10 "http://$host:$mgr_port/processes_list" 2>/dev/null |
        python3 -c 'import json,sys
try:
    data=json.load(sys.stdin)
except Exception:
    raise SystemExit
for p in data.get("processes", []):
    if p.get("name", "").startswith(("eboot", "PPSA")):
        print(p["pid"])' 2>/dev/null); do
    echo "    kill $pid"
    curl -sS -m 15 "http://$host:$mgr_port/process_kill?pid=$pid" >/dev/null 2>&1 || true
done
sleep 5

echo "==> packaging $title_id"
rm -f "$image"
DOTNET_ROOT=/opt/dotnet PATH=/opt/dotnet:$PATH \
    "$ufs2" newfs -D "$dist" "$image" wiliwili >/dev/null

# Drop every other test image so ShadowMountPlus cannot reuse a stale mount.
for old in $(curl -sS --list-only --user anonymous: "ftp://$host:$ftp_port/data/homebrew/" 2>/dev/null |
        grep -E '^PPSA[0-9]+\.ffpkg$' || true); do
    [[ "$old" == "$title_id.ffpkg" ]] && continue
    echo "    removing stale $old"
    curl -sS --user anonymous: -Q "-DELE /data/homebrew/$old" "ftp://$host:$ftp_port/" >/dev/null 2>&1 || true
done
sleep 8

echo "==> uploading $(du -h "$image" | cut -f1)"
curl -sS --fail --connect-timeout 5 --max-time 600 --disable-epsv --user anonymous: \
    --upload-file "$image" "ftp://$host:$ftp_port/data/homebrew/$title_id.ffpkg"

echo "==> waiting for ShadowMountPlus to register the image"
sleep 45

( timeout "$listen_seconds" python3 "$repo/scripts/ps5/native/log-listen.py" "$udp_port" > "$log" 2>&1 & )
sleep 2

echo "==> launching"
launched=0
for attempt in 1 2 3 4 5 6; do
    if curl -sS --fail --connect-timeout 3 --max-time 25 \
            "http://$host:$web_port/launch?titleId=$title_id" >/dev/null 2>&1; then
        launched=1
        break
    fi
    # A 503 usually means "this title is already running" (the previous cycle's
    # process has not exited yet). Treat a live process as a successful launch.
    if curl -sS -m 10 "http://$host:$mgr_port/processes_list" 2>/dev/null |
        grep -q '"name":"eboot'; then
        echo "    launch returned non-success but a title process is running"
        launched=1
        break
    fi
    echo "    launch not ready (attempt $attempt), retrying in 15s"
    sleep 15
done
[[ $launched == 1 ]] || { echo "launch failed after retries" >&2; exit 1; }

sleep $((listen_seconds - 5))
echo "==> log ($log)"
cat "$log"
