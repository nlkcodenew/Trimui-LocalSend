#!/bin/bash
# Trimui-LocalSend.sh — launcher cho mục Tools trên Knulli (Brick Pro).
# App payload nằm tại /userdata/system/trimui-localsend (bin, lib, assets,
# ota-update.sh, VERSION). Binary trimui-localsend là bản build stock OS
# (glibc dynamic) — chạy nguyên vẹn trên Knulli (đủ SDL2/SDL2_ttf/libcurl;
# OpenSSL 1.1 dùng bản bundle trong lib/).
APP=/userdata/system/trimui-localsend
LOG="$APP/localsend.log"

# Binary hardcode đường dẫn /mnt/SDCARD (save sync, download, free-space).
# Knulli không có /mnt/SDCARD -> map symlink sang /userdata (phân vùng data).
if [ ! -e /mnt/SDCARD ]; then
  mkdir -p /mnt 2>/dev/null
  ln -sfn /userdata /mnt/SDCARD 2>/dev/null
fi

export LD_LIBRARY_PATH="$APP/lib:$LD_LIBRARY_PATH"
mkdir -p "$APP/data" /userdata/Download/LocalSend

# Xoá trạng thái OTA "done" của phiên trước (giống launch.sh stock):
# thiếu bước này ota-update.sh sẽ tự thoát ngay (case done*) ở mọi lần
# kiểm tra -> OTA chết vĩnh viễn sau lần cập nhật thành công đầu tiên.
case "$(cat "$APP/.ota-status" 2>/dev/null)" in
  done*) rm -f "$APP/.ota-status" "$APP/.ota-status.tmp" 2>/dev/null;;
esac

# OTA tự động, im lặng — giữ nguyên hành vi bản stock OS.
# Tắt hẳn: TRIMUI_LS_NO_OTA=1 (đặt trong custom.sh nếu muốn).
# QUAN TRỌNG (fix treo khi thoát app trên Knulli): emulatorlauncher chạy
# script này với stdout/stderr là PIPE và communicate() chờ pipe EOF.
# Vòng nền PHẢI redirect cả 3 FD ra khỏi pipe cha, nếu không tiến trình
# sleep/curl mồ côi giữ pipe mở -> ES kẹt sau khi app thoát ("treo").
OTA_PID=""
if [ -z "$TRIMUI_LS_NO_OTA" ] && [ -f "$APP/ota-update.sh" ]; then
  (
    sleep 30
    while :; do
      sh "$APP/ota-update.sh" --apply >> "$APP/ota.log" 2>&1
      sleep 600
    done
  ) </dev/null >> "$APP/ota.log" 2>&1 &
  OTA_PID=$!
fi

"$APP/bin/trimui-localsend" >> "$LOG" 2>&1
CODE=$?

# Dọn vòng OTA: kill subshell + các con trực tiếp của nó (sleep/curl).
# Knulli không có pkill -> quét /proc theo PPID.
if [ -n "$OTA_PID" ]; then
  kill "$OTA_PID" 2>/dev/null
  for p in /proc/[0-9]*; do
    [ -r "$p/stat" ] || continue
    pp=$(awk '{print $4}' "$p/stat" 2>/dev/null)
    [ "$pp" = "$OTA_PID" ] && kill "${p#/proc/}" 2>/dev/null
  done
fi
sync
exit "$CODE"
