#!/usr/bin/env bash
# Build the firmware for every supported board and assemble the browser
# installer site: installer/index.html, one ESP Web Tools manifest per board,
# and downloadable images.
#
# Usage: tools/build_installer.sh [output_dir]     (default: _site)
# Needs arduino-cli with the esp32:esp32 core installed.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-"$ROOT/_site"}
VERSION=$(sed -n 's/^#define VERSION "\(.*\)"$/\1/p' "$ROOT/clocksync.ino")
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT

# id | FQBN | label shown in the installer
BOARDS=(
  "atom|esp32:esp32:m5stack_atom|M5Stack Atom Lite / Matrix / Echo"
  "atoms3|esp32:esp32:m5stack_atoms3|M5Stack AtomS3 / AtomS3 Lite"
  "esp32|esp32:esp32:esp32|ESP32 DevKit"
  "esp32c3|esp32:esp32:esp32c3:CDCOnBoot=cdc|ESP32-C3 (SuperMini and similar)"
  "esp32s3|esp32:esp32:esp32s3|ESP32-S3 DevKit"
)

chip_family() {
  case "$1" in
    esp32) echo "ESP32" ;;
    esp32c3) echo "ESP32-C3" ;;
    esp32c6) echo "ESP32-C6" ;;
    esp32s2) echo "ESP32-S2" ;;
    esp32s3) echo "ESP32-S3" ;;
    *) echo "unknown mcu $1" >&2; exit 1 ;;
  esac
}

prop() {  # prop <properties file> <key>
  sed -n "s/^$2=//p" "$1"
}

rm -rf "$OUT"
mkdir -p "$OUT/firmware" "$OUT/downloads"
boards_json=""

for entry in "${BOARDS[@]}"; do
  IFS='|' read -r id fqbn label <<<"$entry"
  echo "== $label ($fqbn)"
  props="$BUILD/$id.properties"
  arduino-cli compile --fqbn "$fqbn" --show-properties "$ROOT" >"$props"
  arduino-cli compile --fqbn "$fqbn" --build-path "$BUILD/$id" "$ROOT" >/dev/null

  platform=$(prop "$props" runtime.platform.path)
  boot_addr=$(prop "$props" build.bootloader_addr)
  chip=$(chip_family "$(prop "$props" build.mcu)")
  dir="$OUT/firmware/$id"
  mkdir -p "$dir"
  cp "$BUILD/$id/clocksync.ino.bootloader.bin" "$dir/bootloader.bin"
  cp "$BUILD/$id/clocksync.ino.partitions.bin" "$dir/partitions.bin"
  cp "$platform/tools/partitions/boot_app0.bin" "$dir/boot_app0.bin"
  cp "$BUILD/$id/clocksync.ino.bin" "$dir/clocksync.bin"

  # Parts are written separately so the settings region (NVS, 0x9000) is
  # kept unless the user ticks "erase" in the installer.
  cat >"$dir/manifest.json" <<EOF
{
  "name": "clocksync",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "$chip",
      "parts": [
        { "path": "bootloader.bin", "offset": $((boot_addr)) },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "clocksync.bin", "offset": 65536 }
      ]
    }
  ]
}
EOF

  # For other flashing tools: the app alone (write at 0x10000, keeps
  # settings) and a full image (write at 0x0, erases settings).
  cp "$BUILD/$id/clocksync.ino.bin" "$OUT/downloads/clocksync-$VERSION-$id-app.bin"
  cp "$BUILD/$id/clocksync.ino.merged.bin" "$OUT/downloads/clocksync-$VERSION-$id-full.bin"

  [ -n "$boards_json" ] && boards_json+=","
  boards_json+="{\"id\":\"$id\",\"label\":\"$label\",\"chip\":\"$chip\"}"
done

sed "s/__VERSION__/$VERSION/g" "$ROOT/installer/index.html" >"$OUT/index.html"
echo "[$boards_json]" >"$OUT/boards.json"
echo "Installer site for v$VERSION written to $OUT"
