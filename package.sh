#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
VERSION="$(tr -d ' \r\n' < "$ROOT/VERSION")"
STAGE="$ROOT/dist/staging"
OUTPUT="$ROOT/dist/Trimui-LocalSend-v$VERSION.zip"

rm -rf "$STAGE"
mkdir -p "$STAGE/Apps/Trimui-LocalSend"
cp -R "$ROOT/files/." "$STAGE/Apps/Trimui-LocalSend/"
mkdir -p "$STAGE/system/trimui-localsend"
cp -R "$ROOT/files/." "$STAGE/system/trimui-localsend/"
mkdir -p "$STAGE/roms/tools/images"
cp "$ROOT/knulli/Trimui-LocalSend.sh" "$STAGE/roms/tools/Trimui-LocalSend.sh"
cp "$ROOT/files/icon.png" "$STAGE/roms/tools/images/trimui-localsend.png"
cp "$ROOT/HUONG-DAN-CAI-DAT.txt" "$STAGE/HUONG-DAN-CAI-DAT.txt"
chmod +x "$STAGE/Apps/Trimui-LocalSend/launch.sh" \
  "$STAGE/Apps/Trimui-LocalSend/bin/trimui-localsend" \
  "$STAGE/system/trimui-localsend/launch.sh" \
  "$STAGE/system/trimui-localsend/bin/trimui-localsend" \
  "$STAGE/roms/tools/Trimui-LocalSend.sh"

rm -f "$OUTPUT"
cd "$STAGE"
python3 - "$OUTPUT" <<'PY'
import os
import stat
import sys
import zipfile

output = sys.argv[1]
EXEC_NAMES = {"launch.sh", "trimui-localsend", "Trimui-LocalSend.sh"}
with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for root, directories, files in os.walk("."):
        directories.sort()
        files.sort()
        for name in files:
            path = os.path.join(root, name)
            arc = os.path.relpath(path, ".")
            info = zipfile.ZipInfo.from_file(path, arc)
            mode = os.stat(path).st_mode
            if name in EXEC_NAMES:
                mode |= stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
            info.external_attr = (mode & 0xFFFF) << 16
            with open(path, "rb") as source:
                archive.writestr(info, source.read(), compress_type=zipfile.ZIP_DEFLATED,
                                 compresslevel=9)
PY
cd "$ROOT"
rm -rf "$STAGE"
echo "$OUTPUT"
