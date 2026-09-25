#!/usr/bin/env bash
# Package the native title into an ffpkg, upload it, launch it and stream the
# boot log back over UDP.
#
#   run-title.sh <title-id> [seconds]
#
# The image always lands in /data/homebrew, which ShadowMountPlus scans; the
# system partition only has a few MB free and cannot hold the title itself.
set -euo pipefail

title_id=${1:?usage: run-title.sh <title-id> [seconds]}
listen_seconds=${2:-80}
host=${WILIWILI_PS5_HOST:-192.168.102.118}
ftp_port=${WILIWILI_PS5_FTP_PORT:-2120}
web_port=${WILIWILI_PS5_WEB_PORT:-8080}
udp_port=${WILIWILI_LOG_UDP_PORT:-9999}

repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
root=$(dirname -- "$repo")
dist="$repo/build-ps5/native/dist/$title_id"
image="/tmp/$title_id.ffpkg"
log="/tmp/$title_id-boot.log"
ufs2="$repo/build-ps5/pkg-experiment/third_party/ufs2tool/linux-x64/linux-x64/UFS2Tool"

[[ -f "$dist/eboot.bin" ]] || { echo "no eboot.bin under $dist" >&2; exit 1; }
[[ -x "$ufs2" ]] || { echo "missing UFS2Tool at $ufs2" >&2; exit 1; }

echo "==> packaging $title_id"
rm -f "$image"
DOTNET_ROOT=/opt/dotnet PATH=/opt/dotnet:$PATH \
  "$ufs2" newfs -D "$dist" "$image" wiliwili >/dev/null

echo "==> uploading $(du -h "$image" | cut -f1) to /data/homebrew"
curl -sS --fail --connect-timeout 5 --max-time 600 --disable-epsv --user anonymous: \
  --upload-file "$image" "ftp://$host:$ftp_port/data/homebrew/$title_id.ffpkg"

# ShadowMountPlus needs a moment to notice the new image before it can launch.
echo "==> waiting for the mount to settle"
sleep 25

( timeout "$listen_seconds" python3 "$repo/scripts/ps5/native/log-listen.py" "$udp_port" > "$log" 2>&1 & )
sleep 2

echo "==> launching"
curl -sS --fail --connect-timeout 3 --max-time 25 \
  "http://$host:$web_port/launch?titleId=$title_id" >/dev/null

sleep $((listen_seconds - 5))
echo "==> log ($log)"
cat "$log"
