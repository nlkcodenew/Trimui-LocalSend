#!/bin/sh
# ota-update.sh — OTA tự động, IM LẶNG cho Trimui-LocalSend.
# Học từ LottoForecast-Pro (files/ota-update.sh + docs/07):
#   - Tin manifest qua TLS (raw.githubusercontent / jsdelivr)
#   - Binary verify chữ ký Ed25519 của manifest TRƯỚC khi tin bất kỳ
#     hash/path nào (--verify-manifest, dùng OpenSSL; máy không có
#     python3/openssl CLI nên không verify bằng shell được)
#   - Verify sha256 TỪNG FILE trước khi chép
#   - KHÔNG bao giờ đụng tới dữ liệu người dùng: data/ (alias, fingerprint),
#     *.log, .ota-status
#   - Chép atomic từng file (.ota-new -> rename); chmod +x cho *.sh và bin/*
#   - Bản mới có hiệu lực ở lần mở app tiếp theo (không restart giữa chừng
#     để không giết transfer đang chạy)
#
# Dùng: sh ota-update.sh --apply | sh ota-update.sh --check | sh ota-update.sh
# --apply: cài ngay không hỏi (launch.sh gọi nền). --check: chỉ báo có bản mới
# không. Không đối số: hỏi y/N. Tắt hẳn: TRIMUI_LS_NO_OTA=1.
case "$0" in
  */*)
    cd "$(dirname "$0")" || exit 1
    ;;
esac
APP="$(pwd)"
# Đảm bảo binary tìm thấy libcrypto/libssl đi kèm trong lib/ ngay cả khi
# script được chạy tay ngoài launch.sh (khi chạy từ launch.sh thì biến này
# đã được export sẵn và subshell OTA kế thừa).
export LD_LIBRARY_PATH="$APP/lib:/usr/trimui/lib:/usr/lib64:/usr/lib:/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
VERSION_FILE="$APP/VERSION"
CUR="$(cat "$VERSION_FILE" 2>/dev/null | tr -d ' \r\n')"
[ -n "$CUR" ] || CUR="0.0.0"
REPO="nlkcodenew/Trimui-LocalSend"
RAW_BASE="https://raw.githubusercontent.com"
JSDELIVR_MANIFEST="https://cdn.jsdelivr.net/gh/$REPO@main/ota/manifest.json"
LOG="$APP/ota.log"
TMPD="/tmp/trimui-ls-ota-stage"
MANIFEST_JSON="$TMPD/manifest.json"
LOCKD="/tmp/trimui-ls-ota-lock"
OTA_STATUS="$APP/.ota-status"
say() { echo "[ota] $*"; echo "$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
ota_status() {
  printf "%s" "$*" > "$OTA_STATUS.tmp" 2>/dev/null || true
  mv "$OTA_STATUS.tmp" "$OTA_STATUS" 2>/dev/null || rm -f "$OTA_STATUS.tmp" 2>/dev/null || true
}
ota_clear() { rm -f "$OTA_STATUS" "$OTA_STATUS.tmp" 2>/dev/null || true; }
clean_tmp() { rm -rf "$TMPD" 2>/dev/null || true; }
# Dữ liệu người dùng / runtime: không tải, không chép đè.
is_protected() {
  case "$1" in
    data/*|*.log|.ota-status*|*.ota-new) return 0;;
  esac
  return 1
}
# So sánh version dạng x.y... (bỏ hậu tố chữ khi so). Trả 0 = a mới hơn b.
ver_newer() {
  a="$1"; b="$2"
  [ "$a" = "$b" ] && return 1
  i=1
  while :; do
    pa="$(echo "$a" | cut -d. -f$i 2>/dev/null)"; pb="$(echo "$b" | cut -d. -f$i 2>/dev/null)"
    [ -z "$pa$pb" ] && break
    pa="${pa%%[!0-9]*}"; pb="${pb%%[!0-9]*}"
    [ -z "$pa" ] && pa=0; [ -z "$pb" ] && pb=0
    pa="$(echo "$pa" | sed 's/^0*//')"; pb="$(echo "$pb" | sed 's/^0*//')"
    [ -z "$pa" ] && pa=0; [ -z "$pb" ] && pb=0
    if [ "$pa" -gt "$pb" ] 2>/dev/null; then return 0; fi
    if [ "$pa" -lt "$pb" ] 2>/dev/null; then return 1; fi
    i=$((i + 1))
    [ "$i" -gt 8 ] && break
  done
  case "$a" in *"-"*) a_pre=1;; *) a_pre=0;; esac
  case "$b" in *"-"*) b_pre=1;; *) b_pre=0;; esac
  if [ "$a_pre" = "0" ] && [ "$b_pre" = "1" ]; then return 0; fi
  return 1
}
fetch() {
  url="$1"; out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --connect-timeout 8 --max-time 25 --retry 0 -o "$out" "$url" 2>/dev/null && return 0
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -q --timeout=25 --tries=1 -O "$out" "$url" 2>/dev/null && return 0
  fi
  return 1
}
# Tải từng file trong manifest, verify sha256, stage vào $TMPD.
download_files() {
  LIST="$TMPD.files.list"
  sed 's/},/}\n/g' "$MANIFEST_JSON" 2>/dev/null | awk '
    /"path"/ { p=$0; sub(/.*"path"[[:space:]]*:[[:space:]]*"/, "", p); sub(/".*/, "", p) }
    /"sha256"/ { s=$0; sub(/.*"sha256"[[:space:]]*:[[:space:]]*"/, "", s); sub(/".*/, "", s) }
    /"size"/ { z=$0; sub(/.*"size"[[:space:]]*:[[:space:]]*/, "", z); sub(/[^0-9].*/, "", z); if (p != "" && s != "" && z != "") print p "|" s "|" z; p=""; s=""; z="" }
  ' > "$LIST" 2>/dev/null
  [ -s "$LIST" ] || { say "Khong doc duoc danh sach file"; return 1; }
  command -v sha256sum >/dev/null 2>&1 || { say "Thieu sha256sum"; return 1; }
  TOTAL_BYTES="$(awk -F'|' '{ total += $3 } END { printf "%.0f", total }' "$LIST" 2>/dev/null)"
  [ -n "$TOTAL_BYTES" ] || TOTAL_BYTES=0
  DONE_BYTES=0
  while IFS= read -r e; do
    rel="${e%%|*}"; rest="${e#*|}"; want="${rest%%|*}"; size="${rest##*|}"
    [ -n "$rel" ] && [ -n "$want" ] && [ -n "$size" ] || return 1
    case "$rel" in /*|*\\*|../*|*/../*|*/..|..) say "Duong dan khong an toan: $rel"; return 1;; esac
    if is_protected "$rel"; then DONE_BYTES=$((DONE_BYTES + size)); continue; fi
    ota_status "downloading $REM $DONE_BYTES $TOTAL_BYTES $rel"
    got=""
    for b in $BASES; do
      if fetch "$b/$rel" "$TMPD.dl.tmp"; then got="$TMPD.dl.tmp"; break; fi
    done
    [ -n "$got" ] || { say "Khong tai duoc: $rel"; return 1; }
    have="$(sha256sum "$got" 2>/dev/null | cut -d' ' -f1)"
    if [ "$have" != "$want" ]; then say "Sai ma kiem tra: $rel"; rm -f "$got"; return 1; fi
    dst="$TMPD/$rel"
    mkdir -p "$(dirname "$dst")" 2>/dev/null
    mv "$got" "$dst" || return 1
    DONE_BYTES=$((DONE_BYTES + size))
    ota_status "downloading $REM $DONE_BYTES $TOTAL_BYTES $rel"
    say "ok $rel"
  done < "$LIST"
  say "Da tai xong $(wc -l < "$LIST" 2>/dev/null | tr -d ' ') file"
  return 0
}
MODE="${1:-}"
if [ -n "${TRIMUI_LS_NO_OTA:-}" ] && [ "$TRIMUI_LS_NO_OTA" != "0" ]; then
  [ "$MODE" = "--apply" ] || say "Da tat OTA (TRIMUI_LS_NO_OTA=1)."
  exit 0
fi
# "done" cua phien truoc:
#  - done <cung version dang cai> -> update da ap dung xong -> xoa, kiem tra tiep
#  - done <version moi hon>       -> cho restart de ap dung -> bo qua (giua nguyen)
# Tu chua lanh neu launcher nao quen xoa trang thai (da gap tren Knulli 2026-10-20).
case "$(cat "$OTA_STATUS" 2>/dev/null)" in
  "done $CUR") ota_clear;;
  done*) exit 0;;
esac
acquire_lock() {
  if mkdir "$LOCKD" 2>/dev/null; then
    echo "$$" > "$LOCKD/pid" 2>/dev/null
    return 0
  fi
  old="$(cat "$LOCKD/pid" 2>/dev/null)"
  case "$old" in
    ''|*[!0-9]*) ;;
    *) kill -0 "$old" 2>/dev/null && return 1;;
  esac
  rm -rf "$LOCKD" 2>/dev/null
  mkdir "$LOCKD" 2>/dev/null || return 1
  echo "$$" > "$LOCKD/pid" 2>/dev/null
}
if ! acquire_lock; then
  say "Trinh cap nhat dang chay"
  exit 4
fi
trap 'rm -rf "$LOCKD" 2>/dev/null' EXIT INT TERM
clean_tmp
mkdir -p "$TMPD" 2>/dev/null || { say "Khong tao duoc thu muc tam"; exit 1; }
say "local=$CUR"
ota_status "checking"
got_manifest=0
for try in 1 2; do
  if fetch "$RAW_BASE/$REPO/main/ota/manifest.json" "$MANIFEST_JSON"; then
    got_manifest=1; break
  fi
  if fetch "$JSDELIVR_MANIFEST" "$MANIFEST_JSON"; then
    got_manifest=1; break
  fi
  [ "$try" = "1" ] && sleep 3
done
[ "$got_manifest" = "1" ] || { say "Khong tai duoc manifest"; ota_status "failed"; clean_tmp; exit 1; }
# BẮT BUỘC binary verify chữ ký Ed25519 trước khi tin manifest.
if [ ! -x "$APP/bin/trimui-localsend" ] || ! "$APP/bin/trimui-localsend" --verify-manifest "$MANIFEST_JSON" >> "$LOG" 2>&1; then
  say "Chu ky manifest khong hop le"
  ota_status "failed"
  clean_tmp
  exit 1
fi
REM="$(grep -o '"version"[[:space:]]*:[[:space:]]*"[^"]*"' "$MANIFEST_JSON" 2>/dev/null | head -n 1 | sed 's/.*"\([^"]*\)"$/\1/')"
[ -n "$REM" ] || { say "Manifest thieu version"; ota_status "failed"; clean_tmp; exit 1; }
say "remote=$REM"
if ! ver_newer "$REM" "$CUR"; then
  say "Da la ban moi nhat ($CUR)"
  clean_tmp
  ota_status "up_to_date $CUR"
  exit 2
fi
if [ "$MODE" = "--check" ]; then
  say "Co ban moi: $REM (dang dung $CUR)."
  clean_tmp
  ota_status "available $REM"
  exit 10
fi
BASES="$RAW_BASE/$REPO/main/ota/files/v$REM https://cdn.jsdelivr.net/gh/$REPO@main/ota/files/v$REM"
if [ "$MODE" != "--apply" ]; then
  printf "Co ban moi %s (hien tai %s). Cap nhat? [y/N] " "$REM" "$CUR"
  read -r ans
  case "$ans" in y|Y|yes|YES) ;; *) say "Da huy"; clean_tmp; ota_clear; exit 3;; esac
fi
say "Dang tai $REM ..."
ota_status "downloading $REM 0 0 manifest"
if ! download_files; then say "Tai file that bai"; ota_status "failed"; clean_tmp; exit 1; fi
# Cài atomic từng file.
APPLY_FAIL=0
APPLY_TOTAL=0
APPLY_DONE=0
LIST="$TMPD.apply.list"
(cd "$TMPD" && find . -type f -print > "$LIST") || { say "Cai dat that bai"; ota_status "failed"; clean_tmp; exit 1; }
APPLY_TOTAL="$(grep -v -E '^\./[^/]*\.(apply|files)\.list$' "$LIST" 2>/dev/null | grep -v '^\./manifest.json$' | wc -l | tr -d ' ')"
[ -n "$APPLY_TOTAL" ] || APPLY_TOTAL=0
ota_status "installing $REM $APPLY_DONE $APPLY_TOTAL"
while IFS= read -r f; do
  [ -n "$f" ] || continue
  rel="${f#./}"
  case "$rel" in *.apply.list|*.files.list|manifest.json) continue;; esac
  if is_protected "$rel"; then continue; fi
  [ -f "$TMPD/$rel" ] || continue
  dst="$APP/$rel"
  mkdir -p "$(dirname "$dst")" 2>/dev/null
  tmp="$dst.ota-new"
  if ! cp "$TMPD/$rel" "$tmp" 2>/dev/null; then say "Chep that bai: $rel"; APPLY_FAIL=1; break; fi
  case "$rel" in *.sh|bin/*) chmod +x "$tmp" 2>/dev/null;; esac
  if ! mv "$tmp" "$dst" 2>/dev/null; then say "Cai dat that bai: $rel"; APPLY_FAIL=1; break; fi
  APPLY_DONE=$((APPLY_DONE + 1))
  ota_status "installing $REM $APPLY_DONE $APPLY_TOTAL"
  say "installed $rel"
done < "$LIST"
if [ "$APPLY_FAIL" != "0" ]; then ota_status "failed"; clean_tmp; exit 1; fi
cd "$APP" || exit 1
printf "%s" "$REM" | tr -d " \r\n" > "$APP/VERSION" 2>/dev/null
say "Cap nhat xong $CUR -> $REM (co hieu luc o lan mo app tiep theo)."
ota_status "done $REM"
clean_tmp
exit 0
