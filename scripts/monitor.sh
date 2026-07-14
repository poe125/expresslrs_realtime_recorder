#!/bin/bash
# シリアルモニタースクリプト
# Pico のデバッグUART(GP4/5) 出力を表示
# コマンド: d=スナップショット, s=ストリームON/OFF, ?=ヘルプ
# リアルタイムのバーゲージ表示は scripts/live_view.py を使用

set -e

# 第2引数でポート明示指定可: ./monitor.sh [baud] [port]
BAUD_RATE="${1:-921600}"
SERIAL_PORT="${2:-}"

echo "=== ExpressLRS Realtime Recorder Serial Monitor ==="
echo "Baud rate: $BAUD_RATE"
echo "終了するには Ctrl+A, K を押してください (screen の場合)"
echo ""

# PCへのデバッグ出力は GP4/5 → USB-シリアル変換 経由（Picoの native USB は
# ゲームパッドのホスト用で埋まるため）。USB-UART変換の一般的なデバイス名を探す。
if [ -z "$SERIAL_PORT" ]; then
    for pat in \
        /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial* \
        /dev/cu.usbmodem* /dev/tty.usbserial-* /dev/tty.SLAB_USBtoUART* \
        /dev/tty.wchusbserial* /dev/tty.usbmodem*; do
        p=$(ls $pat 2>/dev/null | head -n1)
        if [ -n "$p" ]; then SERIAL_PORT="$p"; break; fi
    done
fi

if [ -z "$SERIAL_PORT" ]; then
    echo "Error: シリアルポートが見つかりません"
    echo ""
    echo "確認事項:"
    echo "  1. USB-シリアル変換アダプタが PC に接続されているか"
    echo "  2. Pico GP4(TX) → 変換の RXD, GND 共通が配線されているか"
    echo ""
    echo "利用可能なシリアルポート:"
    ls /dev/cu.* 2>/dev/null || echo "  (なし)"
    echo ""
    echo "ポートを明示指定するには: ./scripts/monitor.sh $BAUD_RATE /dev/cu.usbserial-XXXX"
    exit 1
fi

echo "Connecting to $SERIAL_PORT ..."
echo ""

# screen を使用してシリアルモニターを開始
screen "$SERIAL_PORT" "$BAUD_RATE"
