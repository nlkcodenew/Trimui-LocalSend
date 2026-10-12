#!/usr/bin/env python3
"""make_manifest.py — sinh + ký Ed25519 ota/manifest.json cho Trimui-LocalSend.

Mô hình học từ LottoForecast-Pro (tools/make_release.py):
  - private key ký release giữ OFFLINE trên máy dev, KHÔNG commit
  - binary trên máy chỉ nhúng public key (src/OtaVerify.cpp)
  - canonical body phải khớp từng byte với OtaVerify.cpp:
      {"files":[{"path":..,"sha256":..,"size":N},...],"kid":"..","version":".."}
    (files sắp xếp theo path, JSON gọn không khoảng trắng)

Dùng:
  python3 tools/make_manifest.py --version 0.3.0 --files files/ \\
      --key ~/.config/trimui-localsend/ota/ls1.key --kid ls1 \\
      --out ota/manifest.json
"""
import argparse
import base64
import hashlib
import json
import os
import sys


def b64url_nopad(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).decode().rstrip("=")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--files", required=True, help="thư mục files/ của release")
    ap.add_argument("--key", required=True, help="private key Ed25519 (PEM, offline)")
    ap.add_argument("--kid", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    from cryptography.hazmat.primitives import serialization

    with open(os.path.expanduser(args.key), "rb") as f:
        priv = serialization.load_pem_private_key(f.read(), password=None)

    entries = []
    for root, _dirs, names in os.walk(args.files):
        for name in names:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, args.files).replace(os.sep, "/")
            if rel.startswith("."):
                continue
            with open(full, "rb") as f:
                data = f.read()
            entries.append({
                "path": rel,
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
            })
    entries.sort(key=lambda e: e["path"])
    if not entries:
        print("không có file nào trong " + args.files, file=sys.stderr)
        return 1

    body_obj = {"files": entries, "kid": args.kid, "version": args.version}
    # ensure_ascii=False: byte UTF-8 đi thẳng, khớp jEscape() bên C++
    # (không escape non-ASCII, chỉ escape ", \, \n, \t, \r).
    body = json.dumps(body_obj, separators=(",", ":"), sort_keys=True,
                      ensure_ascii=False).encode("utf-8")
    # sort_keys=True cho {"files","kid","version"} đúng thứ tự canonical.
    sig = priv.sign(body)
    manifest = dict(body_obj)
    manifest["signature"] = b64url_nopad(sig)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"version={args.version} files={len(entries)} -> {args.out}")
    print(f"canonical_sha256={hashlib.sha256(body).hexdigest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
