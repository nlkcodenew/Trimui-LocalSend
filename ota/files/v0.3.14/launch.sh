#!/bin/sh
# launch.sh — Trimui-LocalSend (TrimUI Brick Pro / Smart Pro S).
case "$0" in
  */*) cd "$(dirname "$0")" || exit 1 ;;
esac

APP="$(pwd)"
export LD_LIBRARY_PATH="$APP/lib:/usr/trimui/lib:/usr/lib64:/usr/lib:/lib:$LD_LIBRARY_PATH"
mkdir -p "$APP/data" /mnt/SDCARD/Download/LocalSend
touch /tmp/stay_alive 2>/dev/null || true

# --- Knulli: chan nac SUSPEND cua idlewatcher khi app dang mo ---
# idlewatcher cua Knulli: sau `timer` giay khong bam phim -> tat man
# (dispoff, khong hai transfer); sau `extendedtimer` giay -> SUSPEND
# ca may -> WiFi chet, transfer dang chay do bi cat. Stock dung file
# /tmp/stay_alive de giu may thuc; Knulli khong co co che inhibit nen
# ta tam day extendedtimer len rat lon khi app mo va tra lai khi thoat
# (chi dung nac suspend — tat man tiet kiem pin van giu nguyen).
KNULLI_IDLE_SAVED="$APP/data/.knulli-idle-saved"
knulli_idle_restore() {
  [ -f "$KNULLI_IDLE_SAVED" ] || return 0
  OLD_EXT="$(cat "$KNULLI_IDLE_SAVED" 2>/dev/null)"
  rm -f "$KNULLI_IDLE_SAVED"
  case "$OLD_EXT" in *[!0-9]*|"") return 0;; esac
  knulli-settings-set system.idlewatcher.extendedtimer "$OLD_EXT" 2>/dev/null
  if [ -f /etc/idlewatcher/idlewatcher.conf ]; then
    sed -i "s/^extended=.*/extended=$OLD_EXT/" /etc/idlewatcher/idlewatcher.conf 2>/dev/null
  fi
  /etc/init.d/S96idlewatcher-daemon restart >/dev/null 2>&1 || true
}
if [ -f /userdata/system/knulli.conf ] && command -v knulli-settings-get >/dev/null 2>&1; then
  knulli_idle_restore  # tu chua neu lan truoc app chet dot tu
  CUR_EXT="$(knulli-settings-get system.idlewatcher.extendedtimer 2>/dev/null)"
  case "$CUR_EXT" in
    *[!0-9]*|"") : ;;
    *)
      if [ "$CUR_EXT" -lt 86400 ]; then
        echo "$CUR_EXT" > "$KNULLI_IDLE_SAVED"
        knulli-settings-set system.idlewatcher.extendedtimer 86400 2>/dev/null
        if [ -f /etc/idlewatcher/idlewatcher.conf ]; then
          sed -i "s/^extended=.*/extended=86400/" /etc/idlewatcher/idlewatcher.conf 2>/dev/null
        fi
        /etc/init.d/S96idlewatcher-daemon restart >/dev/null 2>&1 || true
      fi
      ;;
  esac
fi

# OTA tự động, IM LẶNG (học từ LottoForecast-Pro):
#  - 30s sau khi mở app kiểm tra 1 lần, sau đó mỗi 10 phút kiểm tra lại
#    trong khi app còn mở
#  - tải + cài âm thầm, không hỏi; lỗi -> chỉ ghi vào ota.log
#  - bản mới có hiệu lực ở lần mở app tiếp theo (không restart giữa chừng
#    để không giết transfer đang chạy)
#  - tắt hẳn: TRIMUI_LS_NO_OTA=1
case "$(cat "$APP/.ota-status" 2>/dev/null)" in
  done*) rm -f "$APP/.ota-status" "$APP/.ota-status.tmp" 2>/dev/null;;
esac
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

"$APP/bin/trimui-localsend" >> "$APP/localsend.log" 2>&1
CODE=$?

# Kill vong OTA + cac con (sleep/curl) cua no. Launcher kieu
# emulatorlauncher (Knulli/Batocera) chay script voi stdout/stderr la
# PIPE va cho pipe EOF; tien trinh mo coi giu pipe mo -> ES ket sau khi
# app thoat ("treo"). Knulli khong co pkill -> quet /proc theo PPID.
if [ -n "${OTA_PID:-}" ]; then
  kill "$OTA_PID" 2>/dev/null
  for p in /proc/[0-9]*; do
    [ -r "$p/stat" ] || continue
    pp=$(awk '{print $4}' "$p/stat" 2>/dev/null)
    [ "$pp" = "$OTA_PID" ] && kill "${p#/proc/}" 2>/dev/null
  done
fi
rm -f /tmp/stay_alive 2>/dev/null || true
knulli_idle_restore 2>/dev/null || true
sync
exit "$CODE"
