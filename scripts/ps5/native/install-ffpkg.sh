#!/usr/bin/env bash
# Package the native title as a UFS2 image (.ffpkg) and install it on the console.
#
# A title may not load code at runtime, so the software rendering stack is linked
# into the executable and the image is large. /system_ex has almost no free space
# on this console, so the image lives under /data/homebrew, where ShadowMountPlus
# (app_install_all=1) picks it up and registers it as a title.
#
# usage: install-ffpkg.sh <ps5-host> [title-id]
#
# environment:
#   PS5_FTP_PORT      zftpd port (default 2120)
#   PS5_NATIVE_OUT    build output root (default <repo>/build-ps5/native)
#   PS5_FFPKG_DIR     remote directory (default /data/homebrew)

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <ps5-host> [title-id]" >&2
    exit 2
fi

host=$1
title_id=${2:-PPSA99233}
ftp_port=${PS5_FTP_PORT:-2120}
ftp_user=${PS5_FTP_USER:-anonymous:}
remote_dir=${PS5_FFPKG_DIR:-/data/homebrew}

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
out=${PS5_NATIVE_OUT:-$root/build-ps5/native}
dist=$out/dist/$title_id
image=${PS5_FFPKG_IMAGE:-/tmp/$title_id.ffpkg}
ufs2tool=$root/build-ps5/pkg-experiment/third_party/ufs2tool/linux-x64/linux-x64/UFS2Tool

if [[ ! -d $dist ]]; then
    echo "build output not found: $dist" >&2
    exit 1
fi

# 资源以松散文件随包发布后，镜像里是 313 个小文件，64 KB 块上的开销可观。
# 可用 PS5_FFPKG_NEWFS_ARGS 覆盖块参数（例：-b 8192 -f 1024 -g 16384 -h 64，
# 实测 75 MB → 70 MB）；默认保持工具默认值，避免非默认 fs 参数带来挂载风险。
# **不要**改块参数：2026-10-04 真机实测 `-b 8192 -f 1024 -g 16384 -h 64` 生成的镜像
# 在主机上挂载失败（ShadowMountPlus 日志：image chain mount failed … status=5 I/O error），
# 标题直接起不来（CE-105773-3），而工具默认参数（block 32K）正常。保留覆盖口只是为了
# 以后做实验，默认必须留空。
newfs_args=${PS5_FFPKG_NEWFS_ARGS:-}
echo "==> building $image from $dist ${newfs_args:+(newfs $newfs_args)}"
rm -f "$image"
# shellcheck disable=SC2086  # 有意按空白拆分参数
DOTNET_ROOT=${DOTNET_ROOT:-/opt/dotnet} PATH=/opt/dotnet:$PATH \
    "$ufs2tool" newfs -D "$dist" $newfs_args "$image" wiliwili >/dev/null
ls -la "$image" | awk '{ printf "    image: %.1f MB\n", $5 / 1048576 }'

echo "==> uploading to $remote_dir/$title_id.ffpkg"
curl --fail --connect-timeout 5 --max-time 900 --disable-epsv --ftp-create-dirs \
    --user "$ftp_user" --upload-file "$image" \
    "ftp://$host:$ftp_port$remote_dir/$title_id.ffpkg"

expected=$(stat -c %s "$image")
remote=$(curl --connect-timeout 5 --max-time 60 --disable-epsv --user "$ftp_user" \
    "ftp://$host:$ftp_port$remote_dir/" |
    awk -v name="$title_id.ffpkg" '$NF == name { print $5 }')
if [[ "$remote" != "$expected" ]]; then
    echo "uploaded image is $remote bytes, expected $expected" >&2
    exit 1
fi

echo "==> installed: $remote_dir/$title_id.ffpkg ($remote bytes)"
echo "ShadowMountPlus registers it on its next scan (scan_interval_seconds default 15)."
echo "Then launch with: scripts/ps5/native/launch-native.sh $host $title_id"
