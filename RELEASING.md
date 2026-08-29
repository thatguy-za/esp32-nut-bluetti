# Releasing

Versions come from git tags — ESP-IDF bakes `git describe` into
`esp_app_desc.version`, which the admin page and the OTA UI display.

To cut a release:

```bash
git tag -a vX.Y.Z -m "vX.Y.Z"
git push origin vX.Y.Z

# build from the tagged tree so the image reports the right version
rm -rf build
idf.py build
idf.py merge-bin -o esp32-nut-ecoflow-factory.bin

gh release create vX.Y.Z --prerelease \
  --notes-file - <<'EOF'
...
EOF
gh release upload vX.Y.Z \
  build/esp32-nut-ecoflow-factory.bin build/esp32-nut-ecoflow.bin
```

Rules:

- **Never** `git tag -f` / force-push a tag that's already published.
- One tag per release; bump the version for every change that ships a binary.
- `dist/*.bin` is git-ignored — binaries live only on the release.
- Keep `dist/FLASHING.md`'s size/sha256 in step with the uploaded factory image.

Versioning: `0.MINOR.PATCH` while pre-1.0. Bump MINOR for features, PATCH for
fixes. Stay `--prerelease` until the BLE handshake is confirmed on real
hardware.
