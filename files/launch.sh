#!/bin/sh
case "$0" in
  */*) cd "$(dirname "$0")" || exit 1 ;;
esac

APP="$(pwd)"
export LD_LIBRARY_PATH="$APP/lib:/usr/trimui/lib:/usr/lib64:/usr/lib:/lib:$LD_LIBRARY_PATH"
mkdir -p "$APP/data" /mnt/SDCARD/Download/LocalSend
touch /tmp/stay_alive 2>/dev/null || true

"$APP/bin/trimui-localsend" >> "$APP/localsend.log" 2>&1
CODE=$?

rm -f /tmp/stay_alive 2>/dev/null || true
sync
exit "$CODE"
