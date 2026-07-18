#!/bin/bash
# SDK(TinyUSB)へのワークアラウンドパッチを適用する。
#
# 目的: Logitech F310 (046D:C216) が最初の Interrupt IN 転送を DATA1 PID で始めるため、
#       RP2040 のチップバグ(データシーケンスエラーから回復不可, TinyUSB issue #2776)で
#       EP0x81 がフリーズしレポートが来ない。該当EPの初期PIDを DATA1 にして回避する。
#       詳細は docs/breadboard-bringup.md の Stage C「続報」を参照。
#
# SDK は git 管理外(プロジェクト専用)なので、SDK を入れ直したらこのスクリプトを1回実行する。
# 冪等: 既に適用済みなら何もしない。

set -e

: "${PICO_SDK_PATH:?PICO_SDK_PATH を設定してください（例: export PICO_SDK_PATH=~/pico-sdk）}"

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_FILE="$PROJECT_DIR/patches/hcd_rp2040_f310_data1.patch"
TINYUSB_DIR="$PICO_SDK_PATH/lib/tinyusb"
TARGET="$TINYUSB_DIR/src/portable/raspberrypi/rp2040/hcd_rp2040.c"

if [ ! -f "$PATCH_FILE" ]; then
    echo "Error: パッチが見つかりません: $PATCH_FILE"
    exit 1
fi
if [ ! -f "$TARGET" ]; then
    echo "Error: 対象ファイルが見つかりません: $TARGET"
    echo "  PICO_SDK_PATH ($PICO_SDK_PATH) と TinyUSB の配置を確認してください。"
    exit 1
fi

# 冪等チェック: マーカー文字列があれば適用済み
# （マーカーはパッチが実際に追加する行に含まれる文字列であること。
#   誤ったマーカーだと「未適用」と誤判定し、既適用ファイルへ再適用→逆適用で消える。）
if grep -q "expresslrs_realtime_recorder Stage C" "$TARGET"; then
    echo "パッチは既に適用済みです（skip）: $TARGET"
    exit 0
fi

# 適用可能か事前確認してから当てる。
# -N (--forward): 逆向き/既適用に見えるパッチは黙って無視（誤って -R 逆適用しない安全策）。
if patch -N -d "$TINYUSB_DIR" -p1 --dry-run < "$PATCH_FILE" >/dev/null 2>&1; then
    patch -N -d "$TINYUSB_DIR" -p1 < "$PATCH_FILE"
    echo "パッチを適用しました: hcd_rp2040.c (F310 DATA1 ワークアラウンド)"
else
    echo "Error: パッチを適用できません。SDK/TinyUSB のバージョンが変わった可能性があります。"
    echo "  $TARGET を手動で確認してください（docs/breadboard-bringup.md 参照）。"
    exit 1
fi
