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

rm -rf build && ./tools/package.sh      # builds + names the artifacts by version

gh release create "v$V" --prerelease --notes-file NOTES.md \
  "dist/esp32-nut-ecoflow-$V-factory.bin" "dist/esp32-nut-ecoflow-$V.bin"
```

`tools/package.sh` prints the size and SHA-256 of each artifact (put them in the
release notes) and refuses to run if the built image doesn't report `$V`.

Artifacts:

| file | use |
| --- | --- |
| `esp32-nut-ecoflow-<version>-factory.bin` | first flash over USB, at offset `0x0` |
| `esp32-nut-ecoflow-<version>.bin` | app only — the web OTA update upload |

## Rules

- **Never** `git tag -f` or force-push a published tag. Wrong upload → new patch
  version, new tag.
- One tag per version; the tag name is `v` + `version.txt`.
- `dist/*.bin` is git-ignored — binaries live only on the release.
- Stay `--prerelease` until the BLE handshake is confirmed on real hardware.
