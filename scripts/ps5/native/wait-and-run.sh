#!/usr/bin/env bash
# Wait for the console to come back, restore the payloads, then run one image
# from an already built title directory and stream its boot log back.
#
#   wait-and-run.sh <dist-directory> <title-id> [wait-minutes]
#
# The console drops into rest mode on its own; every service it hosts goes away
# with it, so the whole session has to be re-established before anything can be
# measured. The script logs each stage and leaves the boot log next to the
# capture it produced.
set -euo pipefail

dist=${1:?usage: wait-and-run.sh <dist-directory> <title-id> [wait-minutes]}
title_id=${2:?usage: wait-and-run.sh <dist-directory> <title-id> [wait-minutes]}
wait_minutes=${3:-60}
host=${WILIWILI_PS5_HOST:-192.168.102.118}
ftp_port=${WILIWILI_PS5_FTP_PORT:-2120}
web_port=${WILIWILI_PS5_WEB_PORT:-8080}
admin_port=${WILIWILI_PS5_PAYLOAD_PORT:-8084}
udp_port=${WILIWILI_LOG_UDP_PORT:-9999}

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
image="/tmp/$title_id-run.ffpkg"
log="/tmp/$title_id-run.log"
ufs2="$repo/build-ps5/pkg-experiment/third_party/ufs2tool/linux-x64/linux-x64/UFS2Tool"

port_open() {
    timeout 2 bash -c "echo >/dev/tcp/$host/$1" 2>/dev/null
}

echo "==> waiting for the console's payload manager (up to ${wait_minutes} min)"
for _ in $(seq 1 $((wait_minutes * 6))); do
    if port_open "$admin_port"; then
        echo "==> payload manager is up"
        break
    fi
    sleep 10
done
port_open "$admin_port" || { echo "console never came back" >&2; exit 1; }

echo "==> loading payloads"
for payload in websrv_v0.34.elf zftpd_v1.5.0.elf shsrv_v0.20.elf klogsrv_v0.9.elf; do
    curl -sS --connect-timeout 3 --max-time 20 \
        "http://$host:$admin_port/loadpayload:$payload" >/dev/null 2>&1 || true
    sleep 2
done
for _ in $(seq 1 30); do
    port_open "$ftp_port" && port_open "$web_port" && break
    sleep 2
done
port_open "$ftp_port" || { echo "ftp payload did not start" >&2; exit 1; }

echo "==> packaging $dist"
[[ -f "$dist/eboot.bin" ]] || { echo "no eboot.bin in $dist" >&2; exit 1; }
rm -f "$image"
DOTNET_ROOT=/opt/dotnet PATH=/opt/dotnet:$PATH \
    "$ufs2" newfs -D "$dist" "$image" wiliwili >/dev/null

echo "==> uploading $(du -h "$image" | cut -f1)"
curl -sS --fail --connect-timeout 5 --max-time 600 --disable-epsv --user anonymous: \
    --upload-file "$image" "ftp://$host:$ftp_port/data/homebrew/$title_id.ffpkg"

echo "==> waiting for the mount to settle"
sleep 25

( timeout 90 python3 "$repo/scripts/ps5/native/log-listen.py" "$udp_port" > "$log" 2>&1 & )
sleep 2
echo "==> launching"
curl -sS --connect-timeout 3 --max-time 25 \
    "http://$host:$web_port/launch?titleId=$title_id" >/dev/null 2>&1 || true
sleep 80
echo "==> boot log ($log)"
cat "$log"
