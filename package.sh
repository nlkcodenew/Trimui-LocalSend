#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
VERSION="$(tr -d ' \r\n' < "$ROOT/VERSION")"
STAGE="$ROOT/dist/staging"
OUTPUT="$ROOT/dist/Trimui-LocalSend-stock-v$VERSION.zip"

rm -rf "$STAGE"
mkdir -p "$STAGE/Apps/Trimui-LocalSend"
cp -R "$ROOT/files/." "$STAGE/Apps/Trimui-LocalSend/"
chmod +x "$STAGE/Apps/Trimui-LocalSend/launch.sh" \
  "$STAGE/Apps/Trimui-LocalSend/bin/trimui-localsend"

rm -f "$OUTPUT"
cd "$STAGE"
python3 - "$OUTPUT" <<'PY'
import os
import stat
import sys
import zipfile

output = sys.argv[1]
with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for root, directories, files in os.walk("Apps"):
        directories.sort()
        files.sort()
        for name in files:
            path = os.path.join(root, name)
            info = zipfile.ZipInfo.from_file(path, path)
            mode = os.stat(path).st_mode
            if name == "launch.sh" or path.endswith("/bin/trimui-localsend"):
                mode |= stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH
            info.external_attr = (mode & 0xFFFF) << 16
            with open(path, "rb") as source:
                archive.writestr(info, source.read(), compress_type=zipfile.ZIP_DEFLATED,
                                 compresslevel=9)
PY
cd "$ROOT"
rm -rf "$STAGE"
echo "$OUTPUT"
