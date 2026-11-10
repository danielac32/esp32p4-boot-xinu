#!/usr/bin/env bash
# deploy_app.sh <appdir> [host]  (sube /apps/<appdir>/app.elf e icon565.bin si existe)
set -e
APP="$1"; HOST="${2:-http://192.168.1.107}"
BASE="$(cd "$(dirname "$0")/../apps/$APP" && pwd)"
up(){ p="$1"; f="$2"; [ -f "$f" ] || return 0; curl -s -m 20 -X DELETE "$HOST/api/file?path=$p" >/dev/null || true; sleep 1; curl -s -m 90 -X PUT -T "$f" "$HOST/api/file?path=$p"; echo " <- $p"; }
up "/apps/$APP/app.elf" "$BASE/$APP.elf"
up "/apps/$APP/icon565.bin" "$BASE/icon565.bin"
