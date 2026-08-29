#!/usr/bin/env bash
# Build and stage release artifacts named with the version from version.txt:
#
#   dist/esp32-nut-bluetti-<version>-factory.bin   flash at 0x0 (USB, first time)
#   dist/esp32-nut-bluetti-<version>.bin           app only (web OTA update)
#
# Assumes the ESP-IDF environment is already exported.
set -euo pipefail
cd "$(dirname "$0")/.."

V=$(tr -d '[:space:]' < version.txt)
[ -n "$V" ] || { echo "version.txt is empty" >&2; exit 1; }

idf.py build
idf.py merge-bin -o "esp32-nut-bluetti-factory.bin" >/dev/null

# Confirm the image really reports the version we're about to name it.
# esp_app_desc_t lives at 0x20 (image header 0x18 + segment header 0x8);
# its `version` field is 32 bytes at +0x10.
EMBEDDED=$(python3 -c "
import sys
d = open('build/esp32-nut-bluetti.bin','rb').read()
print(d[0x30:0x50].split(b'\x00')[0].decode('ascii','replace'))
")
if [ "$EMBEDDED" != "$V" ]; then
  echo "built image reports '$EMBEDDED' but version.txt says '$V'" >&2
  echo "(stale build? try: rm -rf build)" >&2
  exit 1
fi

mkdir -p dist
cp build/esp32-nut-bluetti-factory.bin "dist/esp32-nut-bluetti-$V-factory.bin"
cp build/esp32-nut-bluetti.bin         "dist/esp32-nut-bluetti-$V.bin"
cp build/bootloader/bootloader.bin      dist/bootloader.bin
cp build/partition_table/partition-table.bin dist/partition-table.bin

for f in "dist/esp32-nut-bluetti-$V-factory.bin" "dist/esp32-nut-bluetti-$V.bin"; do
  printf '%s  %s bytes  sha256 %s\n' "$(basename "$f")" \
    "$(wc -c < "$f" | tr -d ' ')" \
    "$(python3 -c "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$f")"
done
