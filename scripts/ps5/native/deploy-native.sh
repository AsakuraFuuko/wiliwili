#!/usr/bin/env bash
# Deploy a built PS5 native title to the console.
#
# A title must be installed as a real directory: /system_ex/app/<TITLE_ID>/
# holds the executable, modules and (for the shell) metadata, and
# /user/app/<TITLE_ID>/sce_sys/ holds the user-side param.json and icon. Titles
# that are only reachable through a mount point (for example a ShadowMountPlus
# nullfs link from /data/homebrew) are executed by the loader on a path it
# refuses, which surfaces as SIGSYS in SceSysCore and error CE-107750-0.
#
# The title still has to be registered in the shell database once; ShadowMountPlus
# does that when it installs the folder form, after which this layout replaces the
# mounted copy.
#
# usage: deploy-native.sh <ps5-host> [title-id]
#
# environment:
#   PS5_NATIVE_OUT         build output directory (default <repo>/build-ps5/native)
#   WILIWILI_PS5_FTP_PORT  console FTP port (default 2120)
#   WILIWILI_PS5_FTP_USER  FTP credentials (default anonymous:)

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <ps5-host> [title-id]" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out=${PS5_NATIVE_OUT:-$root/build-ps5/native}
host=$1
title_id=${2:-PPSA99010}
ftp_port=${WILIWILI_PS5_FTP_PORT:-2120}
ftp_user=${WILIWILI_PS5_FTP_USER:-anonymous:}
dist=$out/dist/$title_id

for required in eboot.bin sce_sys/param.json sce_module/libc.prx; do
    [[ -f $dist/$required ]] || {
        echo "missing $dist/$required; run scripts/ps5/native/build-native.sh first" >&2
        exit 1
    }
done

upload() {
    local file=$1 remote=$2
    curl --fail --connect-timeout 5 --max-time 900 --disable-epsv --ftp-create-dirs \
        --user "$ftp_user" --upload-file "$file" "ftp://$host:$ftp_port$remote"
}

# Application image first, executable last: the loader must never see a title
# whose eboot.bin is newer than the rest of the payload.
mapfile -t files < <(cd "$dist" && find . -type f ! -name eboot.bin | sort)
for file in "${files[@]}"; do
    upload "$dist/${file#./}" "/system_ex/app/$title_id/${file#./}"
done

# Shell-side metadata lives outside the application image.
upload "$dist/sce_sys/param.json" "/user/app/$title_id/sce_sys/param.json"
[[ -f $dist/sce_sys/icon0.png ]] &&
    upload "$dist/sce_sys/icon0.png" "/user/app/$title_id/sce_sys/icon0.png"

# The console caches the executable; overwriting it in place can keep serving
# the previous pages, so the old file is removed first.
curl --connect-timeout 5 --max-time 60 -sS --disable-epsv --user "$ftp_user" \
    -Q "DELE /system_ex/app/$title_id/eboot.bin" "ftp://$host:$ftp_port/" >/dev/null 2>&1 || true
upload "$dist/eboot.bin" "/system_ex/app/$title_id/eboot.bin"

# FTP has no checksum command, so the transfer is confirmed by size: a truncated
# upload leaves an eboot the loader rejects without a diagnosis.
expected=$(stat -c %s "$dist/eboot.bin")
remote=$(curl --connect-timeout 5 --max-time 60 --disable-epsv \
    --user "$ftp_user" "ftp://$host:$ftp_port/system_ex/app/$title_id/" |
    awk '$NF == "eboot.bin" { print $5 }')
if [[ "$remote" != "$expected" ]]; then
    echo "eboot.bin on the console is $remote bytes, expected $expected" >&2
    exit 1
fi

# A folder-form title discovered by ShadowMountPlus is mounted over the
# application image at launch, and the loader refuses to execute from that
# mount. Removing the managed source leaves the shell registration intact while
# the real directory stays authoritative.
for stale in eboot.bin sce_sys/param.json; do
    curl --connect-timeout 5 --max-time 60 -sS --disable-epsv \
        -Q "DELE /data/homebrew/$title_id/$stale" "ftp://$host:$ftp_port/" >/dev/null 2>&1 || true
done

echo "installed $title_id as a real title directory on $host"
echo "launch it with: scripts/ps5/native/launch-native.sh $host $title_id"
