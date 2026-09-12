#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

export CGO_ENABLED=0
LDFLAGS="-s -w"

build() {
  local name="$1" arch="$2"
  [ $# -ge 3 ] && arm="$3" || arm=""
  echo "[+] building flood-$name (linux/$arch)"
  GOOS=linux GOARCH="$arch" GOARM="$arm" \
    go build -trimpath -ldflags="$LDFLAGS" -o "flood-$name" .
  file "flood-$name"
}

build x86_64 amd64
build aarch64 arm64
build armv7l arm 7
build x86 386

echo "[+] syncing to methods_cpp/ and bots/"
ROOT="$(cd ../. && pwd)"
for a in x86_64 aarch64 armv7l x86; do
  cp -f "flood-$a" "$ROOT/methods_cpp/flood-$a"
  cp -f "flood-$a" "$ROOT/bots/flood-$a"
done
cp -f flood-x86_64 "$ROOT/methods_cpp/flood"
cp -f flood-x86_64 "$ROOT/bots/flood"
echo "[+] done"