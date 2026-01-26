#!/bin/bash
# Flash書き込みスクリプト
# Picoをbootモード（BOOTSELボタンを押しながらUSB接続）にしてから実行

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
UF2_FILE="$BUILD_DIR/expresslrs_realtime_recorder.uf2"

# Picoのマウントポイント（macOS）
PICO_MOUNT="/Volumes/RPI-RP2"

echo "=== ExpressLRS Realtime Recorder Flash Tool ==="

# ビルド済みか確認
if [ ! -f "$UF2_FILE" ]; then
    echo "Error: $UF2_FILE が見つかりません"
    echo "先に /build を実行してください"
    exit 1
fi

# Picoがマウントされているか確認
if [ ! -d "$PICO_MOUNT" ]; then
    echo "Error: Pico が見つかりません"
    echo ""
    echo "手順:"
    echo "  1. Pico の BOOTSEL ボタンを押しながら USB 接続"
    echo "  2. RPI-RP2 としてマウントされたことを確認"
    echo "  3. このスクリプトを再実行"
    exit 1
fi

echo "Pico detected at $PICO_MOUNT"
echo "Flashing $UF2_FILE ..."

cp "$UF2_FILE" "$PICO_MOUNT/"

echo ""
echo "Flash complete!"
echo "Pico は自動的に再起動します"
