#!/bin/bash
# シリアルモニタースクリプト
# Pico のデバッグUART(GP4/5) 出力を表示
# コマンド: d=スナップショット, s=ストリームON/OFF, ?=ヘルプ
# リアルタイムのバーゲージ表示は scripts/live_view.py を使用

set -e

BAUD_RATE="${1:-921600}"

echo "=== ExpressLRS Realtime Recorder Serial Monitor ==="
echo "Baud rate: $BAUD_RATE"
echo "終了するには Ctrl+A, K を押してください (screen の場合)"
echo ""

# macOS でPico のシリアルポートを探す
SERIAL_PORT=$(ls /dev/tty.usbmodem* 2>/dev/null | head -n1)

if [ -z "$SERIAL_PORT" ]; then
    # cu デバイスも試す
    SERIAL_PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -n1)
fi

if [ -z "$SERIAL_PORT" ]; then
    echo "Error: Pico のシリアルポートが見つかりません"
    echo ""
    echo "確認事項:"
    echo "  1. Pico が USB 接続されているか"
    echo "  2. ファームウェアが USB CDC を有効にしているか"
    echo ""
    echo "利用可能なシリアルポート:"
    ls /dev/tty.* 2>/dev/null || echo "  (なし)"
    exit 1
fi

echo "Connecting to $SERIAL_PORT ..."
echo ""

# screen を使用してシリアルモニターを開始
screen "$SERIAL_PORT" "$BAUD_RATE"
