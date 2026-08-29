# Versioning & releases

The version lives in [`version.txt`](version.txt). ESP-IDF bakes it into
`esp_app_desc.version`, which the admin page and the OTA update UI show.

## Every commit bumps it

`version.txt` is bumped in the **same commit** as the change, so no two builds
ever report the same version:

- **patch** (`0.2.0` → `0.2.1`) — fixes, docs, tests, refactors
- **minor** (`0.2.0` → `0.3.0`) — user-visible features
- **major** — after 1.0

## Cutting a release

A release is a commit whose `version.txt` you also tag:

```bash
# version.txt already bumped and committed
V=$(cat version.txt)
git tag -a "v$V" -m "v$V"
git push origin main "v$V"

rm -rf build && idf.py build            # image reports v$V
idf.py merge-bin -o esp32-nut-ecoflow-factory.bin
# update dist/FLASHING.md size + sha256 to match

gh release create "v$V" --prerelease --notes-file NOTES.md
gh release upload  "v$V" \
  build/esp32-nut-ecoflow-factory.bin build/esp32-nut-ecoflow.bin
```

## Rules

- **Never** `git tag -f` or force-push a published tag. Wrong upload → new patch
  version, new tag.
- One tag per version; the tag name is `v` + `version.txt`.
- `dist/*.bin` is git-ignored — binaries live only on the release.
- Stay `--prerelease` until the BLE handshake is confirmed on real hardware.
