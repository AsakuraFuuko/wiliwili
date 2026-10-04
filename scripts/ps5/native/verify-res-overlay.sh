#!/usr/bin/env bash
# 资源 overlay（松散文件优先、内嵌 romfs 回退）端到端验证。
#
# 背景：标题沙箱对任何路径的 libc opendir() 都返回 EPERM，所以目录遍历必须用
# open(O_RDONLY|O_DIRECTORY) + getdents() 自己实现；资源树以松散文件随包发布到
# 镜像内 /app0/assets/，运行时文件优先、内嵌 romfs 回退。故意不使用 /download0。
# 这个脚本把"构建 → 校验产物 → 部署抓日志 → 判定"做成可重复的一轮。
#
# usage: verify-res-overlay.sh [--skip-build] [--host <ip>]
#
#   --skip-build   复用现有 build-ps5/native/dist/<title>/，不重新构建（迭代时省时间）
#   --host <ip>    覆盖默认主机 192.168.102.118
#
# 环境变量：
#   PS5_NATIVE_TITLE_ID  标题号（默认 PPSA99233）
#   WILIWILI_LOG_LINES   从启动日志尾部取多少行（默认这里提到 600，避免 res-dir 被截掉）
#
# 任何一步失败都非零退出并打印原因。脚本可重复重跑：临时文件都在 /tmp 且退出时清理。
# 主机休眠时 test-cycle/app-log 会连接失败；脚本只提示"需先唤醒主机"，不自动唤醒。
set -euo pipefail

TITLE_ID=${PS5_NATIVE_TITLE_ID:-PPSA99233}
HOST=192.168.102.118
SKIP_BUILD=0
LISTEN_SECONDS=30

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --host)
            [[ $# -ge 2 ]] || die "--host 需要一个 IP 参数"
            HOST=$2
            shift 2
            ;;
        --host=*)
            HOST=${1#*=}
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

# 无论从哪个目录调用都锚定到仓库根（build/test-cycle/app-log 各自也会自算 root）。
root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
cd "$root"

dist_assets="$root/build-ps5/native/dist/$TITLE_ID/assets"
res_dir="$root/resources"

[[ -d "$res_dir" ]] || die "找不到资源树：$res_dir"

# 临时 options 与日志；同一 trap 统一清理，保证可重复重跑不残留。
options=$(mktemp "/tmp/wiliwili-res-overlay-XXXXXX.txt")
applog=$(mktemp "/tmp/wiliwili-res-overlay-log-XXXXXX.txt")
trap 'rm -f "$options" "$applog"' EXIT

echo "================ 资源 overlay 验证 ================"
echo "标题: $TITLE_ID   主机: $HOST"

# ---------- 1) 构建 ----------
# 先记录 HEAD 与工作区状态：否则无法判断即将验证的产物到底对应哪份源码。
echo
echo "==> [1/5] 构建前仓库状态"
if git -C "$root" rev-parse --git-dir >/dev/null 2>&1; then
    echo "    HEAD: $(git -C "$root" rev-parse --short HEAD) $(git -C "$root" log -1 --format=%s)"
    if [[ -n "$(git -C "$root" status --porcelain)" ]]; then
        echo "    警告：工作区不干净，产物可能包含未提交改动："
        git -C "$root" status --porcelain | sed 's/^/      /'
    else
        echo "    工作区干净"
    fi
else
    echo "    警告：$root 不是 git 工作树，无法报告 HEAD"
fi

if [[ $SKIP_BUILD == 1 ]]; then
    echo
    echo "==> [1/5] 跳过构建（--skip-build），复用 $root/build-ps5/native/dist/$TITLE_ID"
    [[ -f "$root/build-ps5/native/dist/$TITLE_ID/eboot.bin" ]] ||
        die "没有可复用的构建产物：$root/build-ps5/native/dist/$TITLE_ID/eboot.bin"
else
    echo
    echo "==> [1/5] 构建（PS5_NATIVE_AGC=1, PS5_NATIVE_TITLE_ID=$TITLE_ID）"
    # 仓库既定构建命令，见 AGENTS.md「原生标题线（正式产物 PPSA99233）」。
    # -u 清掉可能残留的 OSMesa 开关，避免误走历史软渲染分支。
    env -u PS5_NATIVE_OSMESA_DIR -u PS5_NATIVE_OSMESA_LLVM PS5_NATIVE_AGC=1 \
        PS5_NATIVE_SDL2_PREFIX="$root/../ps5-native/ps5-opengl/build/native-sdl2-audio/sdk" \
        PS5_NATIVE_CDB="$root/build-ps5/compile_commands.json" \
        PS5_NATIVE_TITLE_ID="$TITLE_ID" \
        bash "$root/scripts/ps5/native/build-native.sh"
fi

# ---------- 2) 校验产物 ----------
# 松散资源必须真的进包：逐一对齐 resources/ 的顶层目录，再比较文件计数。
# 目录/ca 缺失属于硬失败（overlay 没生效）；计数不一致先告警，因为可能只是
# 构建脚本过滤了少量文件，需要人工判断。
echo
echo "==> [2/5] 校验 $dist_assets"
[[ -d "$dist_assets" ]] || die "产物目录不存在：$dist_assets"
[[ -f "$dist_assets/ca-bundle.crt" ]] || die "缺少 ca-bundle.crt：$dist_assets/ca-bundle.crt"

res_dirs=()
for d in "$res_dir"/*/; do
    [[ -d "$d" ]] || continue
    res_dirs+=("$(basename "$d")")
done
[[ ${#res_dirs[@]} -gt 0 ]] || die "$res_dir 下没有任何资源子目录"

missing=0
for d in "${res_dirs[@]}"; do
    if [[ -d "$dist_assets/$d" ]]; then
        echo "    [OK] $d/"
    else
        echo "    [MISSING] $d/" >&2
        missing=1
    fi
done
[[ $missing == 0 ]] || die "产物缺少资源目录（overlay 未生效）"

# 全套递归对比：dist 里必须**不缺** resources/ 的任何文件。dist 额外允许
# ca-bundle.crt（裁剪 CA）与 wiliwili-options.txt（test-cycle 写入的探针开关）。
res_count=$(find "$res_dir" -type f | wc -l)
mapfile -t missing_files < <(
    while IFS= read -r rel; do
        [[ -f "$dist_assets/$rel" ]] || printf '%s\n' "$rel"
    done < <(cd "$res_dir" && find . -type f | sed 's|^\./||')
)
out_count=$(find "$dist_assets" -type f | wc -l)
echo "    松散文件计数：resources/=$res_count  dist/assets/=$out_count（后者含 CA 与 options）"
if ((${#missing_files[@]} > 0)); then
    echo "    警告：dist 缺少 ${#missing_files[@]} 个资源文件（前 10 个）："
    printf '          %s\n' "${missing_files[@]:0:10}"
fi

# ---------- 3) 部署并抓日志 ----------
# overlay 探针（含 res-dir:）与 crypto 探针共用 WILIWILI_CRYPTO_PROBE=1 开关，
# 通过镜像内的 assets/wiliwili-options.txt 传入（test-cycle 会把它复制进 dist）。
echo
echo "==> [3/5] 部署并抓日志（test-cycle $TITLE_ID $LISTEN_SECONDS）"
printf 'WILIWILI_CRYPTO_PROBE=1\n' > "$options"

# 先探一次 TCP；主机休眠时给出明确提示，而不是让后续 curl 报一堆超时。
if ! curl -sS -m 8 -o /dev/null "http://$HOST:8080/" 2>/dev/null; then
    echo "    警告：$HOST:8080 不可达；若主机处于休眠请先唤醒（脚本不会自动唤醒）。" >&2
fi

if ! bash "$root/scripts/ps5/native/test-cycle.sh" "$TITLE_ID" "$LISTEN_SECONDS" "$options"; then
    echo "!!! test-cycle 失败。" >&2
    echo "    若报错为连接超时/拒绝（curl: (7)/(28) 等），主机很可能处于休眠。" >&2
    echo "    请先唤醒 PS5 后重试（必要时加 --skip-build 复用已构建产物）。" >&2
    exit 1
fi

# ---------- 4) 从启动日志判定 ----------
echo
echo "==> [4/5] 取启动日志并判定"
if ! WILIWILI_LOG_LINES=${WILIWILI_LOG_LINES:-600} \
    bash "$root/scripts/ps5/native/app-log.sh" "$HOST" "$TITLE_ID" > "$applog" 2>&1; then
    echo "!!! app-log.sh 失败（主机可能休眠，或 download0 不可达）。原始输出：" >&2
    cat "$applog" >&2
    exit 1
fi

fail=0

# (a) 目录遍历已实现：必须出现 res-dir: 行，并把 count/first 打出来。
if grep -q 'res-dir:' "$applog"; then
    echo "    [OK] 出现 res-dir:"
    grep 'res-dir:' "$applog" | sed 's/^/        /'
    # 只在 res-dir: 行内取值——日志里 img-net: 行也有 first=，混着取会读错。
    res_line=$(grep -m1 'res-dir:' "$applog" || true)
    res_count_field=$(printf '%s\n' "$res_line" | grep -oE 'count=[0-9]+' || true)
    res_first_field=$(printf '%s\n' "$res_line" | grep -oE 'first=[^ ]+' || true)
    echo "        -> ${res_count_field:-count=<未找到>}  ${res_first_field:-first=<未找到>}"
else
    echo "    [FAIL] 未出现 res-dir:（目录遍历未实现、未触发，或日志被截断）" >&2
    fail=1
fi

# (b) 不得回退失败：资源路径必须能被 overlay 解析，绝不能抛 Invalid romfs resource path。
if grep -q 'Invalid romfs resource path' "$applog"; then
    echo "    [FAIL] 出现 Invalid romfs resource path：" >&2
    grep 'Invalid romfs resource path' "$applog" | sed 's/^/        /' >&2
    fail=1
else
    echo "    [OK] 无 Invalid romfs resource path"
fi

# (c) 网络仍然正常：至少要有一条成功的 HTTP 响应。
if grep -q 'http: code=200' "$applog"; then
    echo "    [OK] 出现 http: code=200"
else
    echo "    [FAIL] 未出现 http: code=200（网络/API 可能被破坏或未加载）" >&2
    fail=1
fi

# ---------- 5) 摘要 ----------
echo
echo "================ 验证摘要 ================"
echo "  标题:       $TITLE_ID"
echo "  主机:       $HOST"
if [[ $SKIP_BUILD == 1 ]]; then
    echo "  构建:       跳过（复用 dist）"
else
    echo "  构建:       本次已执行"
fi
echo "  松散文件:   resources=$res_count  dist=$out_count（dist 含 CA 与 options）"
if ((${#missing_files[@]} == 0)); then
    echo "  资源齐全:   是（dist 覆盖 resources 全部文件）"
else
    echo "  资源齐全:   否（缺 ${#missing_files[@]} 个，见上方告警）"
fi
if [[ $fail == 0 ]]; then
    echo "  判定:       PASS"
else
    echo "  判定:       FAIL"
fi
echo "=========================================="

exit "$fail"
