#!/usr/bin/env bash
# 生成标题用的裁剪 CA bundle。
#
# 为什么裁剪：libcurl 每条**新建**连接都会为 easy 句柄新建 SSL_CTX 并重新解析
# 整个 CA 文件；真机实测（notes/06 §10.29）在标题运行时里这个调用会被进程内共享
# 资源串行化——单条 11 ms、顺序 5 次 55 ms，而并发 5 次是 2.2 s（同刻完成），
# 页面加载一次开 5–6 条新连接就是"图片/接口间歇 2 s"。并发代价与证书数量近似线性：
# 140 张 → 2214 ms、40 张 → 680 ms、5 张 → 111 ms。所以只保留 B 站实际用到的 CA，
# TLS 校验（SSL_VERIFY_PEER）保持开启。
#
# B 站链路实测（2026-10-04，PC openssl s_client）：
#   *.hdslb.com        → GlobalSign Root R46 / GlobalSign Root CA - R3
#   api/passport/app.bilibili.com, grpc.biliapi.net → GlobalSign Root CA - R3
# 另外保留几张常见根（DigiCert G2、ISRG X1、Sectigo R46、Amazon R1）作为兜底。
set -euo pipefail

sdk=${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}
src="$sdk/target/user/homebrew/etc/ca-bundle.crt"
out="${1:-$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/ca-bundle-trimmed.crt}"

[[ -f "$src" ]] || { echo "missing source bundle: $src" >&2; exit 1; }

# 生成方式：先用 openssl s_client 拉取 B 站各主机的证书链，取每条链最顶层证书的
# SHA-256 指纹作为"必须保留的信任锚"，再从 SDK bundle 里按指纹挑出来；另外按
# subject 保留几张常见根作为兜底。下面只实现按 subject 的兜底名单部分，指纹部分
# 依赖网络，需要时按上面的方法手动跑一次并把结果覆盖到本文件同目录的
# ca-bundle-trimmed.crt（当前编号：7 张证书 / ~10 KB，覆盖 hdslb/bilibili/bilivideo/akamai）。
python3 - "$src" "$out" <<'PY'
import subprocess, sys, tempfile, os

src, out = sys.argv[1], sys.argv[2]
wanted = [
    "GlobalSign Root CA - R3",
    "GlobalSign Root R46",
    "GlobalSign Root CA - R6",
    "GlobalSign Root E46",
    "GlobalSign Root CA",
    "DigiCert Global Root G2",
    "DigiCert Global Root CA",
    "ISRG Root X1",
    "Sectigo Public Server Authentication Root R46",
    "Amazon Root CA 1",
]

blocks, current = [], []
for line in open(src, encoding="utf-8", errors="ignore"):
    current.append(line)
    if "-----END CERTIFICATE-----" in line:
        blocks.append("".join(current))
        current = []

kept, names = [], []
with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "cert.pem")
    for block in blocks:
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(block)
        subject = subprocess.run(["openssl", "x509", "-noout", "-subject", "-in", path],
                                 capture_output=True, text=True).stdout
        for name in wanted:
            if f"CN={name}" in subject or f"CN = {name}" in subject:
                kept.append(block)
                names.append(name)
                break

with open(out, "w", encoding="utf-8") as handle:
    handle.write("".join(kept))
print(f"kept {len(kept)}/{len(blocks)} certificates -> {out}")
missing = [n for n in wanted if n not in names]
print("matched: " + ", ".join(sorted(set(names))))
if missing:
    print("absent:  " + ", ".join(missing))
PY
