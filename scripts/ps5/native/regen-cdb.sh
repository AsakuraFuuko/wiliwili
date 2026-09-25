#!/usr/bin/env bash
# 生成 native 构建需要的 compile_commands.json。
#
# 背景（踩过的坑，别再踩）：
#   * payload 构建本身不产 compile_commands.json；
#   * `ninja -t compdb` 的版本会把依赖文件参数（-MD/-MT/-MF）也写进 command，
#     而 native 构建用 clang-18 + -Werror=unused-command-line-argument，会直接报错；
#   * ninja 输出的相对路径是相对**构建目录**的，而 native_build.py 按仓库根解析；
#   * libromfs 的资源 TU（libromfs_resources.cpp）必须排除，否则会与链接进来的
#     libromfs-wiliwili.a 符号重复（RomFs_wiliwili_*）。
#
# 用法：bash scripts/ps5/native/regen-cdb.sh [仓库根]
set -euo pipefail

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
build_dir="$root/build-ps5"
cd "$root"

PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}" bash scripts/ps5/build.sh >/tmp/wiliwili-payload-build.log 2>&1

ninja -C "$build_dir" -t compdb >"$build_dir/compile_commands.json"

python3 - "$root" "$build_dir/compile_commands.json" <<'PY'
import json, os, sys

root, path = sys.argv[1], sys.argv[2]
entries = json.load(open(path))
DROP = {"-MD", "-MMD", "-M"}
DROP_ARG = {"-MT", "-MF", "-MQ"}

cleaned = []
for entry in entries:
    if not isinstance(entry.get("command"), str):
        continue
    tokens = entry["command"].split()
    out, i = [], 0
    while i < len(tokens):
        token = tokens[i]
        if token in DROP:
            i += 1
            continue
        if token in DROP_ARG:
            i += 2
            continue
        out.append(token)
        i += 1
    entry["command"] = " ".join(out)

    # ninja 的 compdb 不带规则过滤，会把链接/自定义命令也算进来；只保留真正的源码。
    if not entry["file"].endswith((".c", ".cc", ".cpp", ".cxx")):
        continue

    source = entry["file"]
    if not os.path.isabs(source):
        candidate = os.path.join(root, "build-ps5", source)
        if os.path.exists(candidate):
            entry["file"] = candidate
        else:
            continue
    if "libromfs_resources.cpp" in entry["file"]:
        continue  # 资源来自 libromfs-wiliwili.a，不能重复编译
    cleaned.append(entry)

json.dump(cleaned, open(path, "w"), indent=1)
print("compile_commands.json: %d entries" % len(cleaned))
PY
