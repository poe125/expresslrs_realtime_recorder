#!/bin/bash
# Flash書き込みスクリプト
# Picoをbootモード（BOOTSELボタンを押しながらUSB接続）にしてから実行

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
UF2_FILE="$BUILD_DIR/expresslrs_realtime_recorder.uf2"

# Picoのマウントポイント（macOS/Linux両対応で探索）
PICO_MOUNT=""
for cand in /Volumes/RPI-RP2 "/media/$USER/RPI-RP2" "/run/media/$USER/RPI-RP2" /media/RPI-RP2; do
    if [ -d "$cand" ]; then PICO_MOUNT="$cand"; break; fi
done

echo "=== ExpressLRS Realtime Recorder Flash Tool ==="

# ビルド済みか確認
if [ ! -f "$UF2_FILE" ]; then
    echo "Error: $UF2_FILE が見つかりません"
    echo "先に /build を実行してください"
    exit 1
fi

# Picoがマウントされているか確認
if [ -z "$PICO_MOUNT" ]; then
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
