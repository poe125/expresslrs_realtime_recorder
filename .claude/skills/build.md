# /build - Pico SDK ビルドスキル

## 概要
Raspberry Pi Pico用ファームウェアをビルドします。

## 実行手順

1. buildディレクトリが存在しない場合は作成
2. CMakeでビルドシステムを生成
3. makeでビルドを実行

## ビルド環境（このマシンに構築済み）
- **Pico SDK**: `~/pico-sdk`（SDK 2.2.0, tinyusb 0.18.0 サブモジュール込み）
- **ARMツールチェーン**: `~/gcc-arm-none-eabi/bin`（Arm GNU Toolchain 14.2.Rel1, tar.xz展開・sudo不要）
- cmake は homebrew 版を使用

再構築が必要な場合:
```bash
# ツールチェーン（newlib同梱の公式版。brew formula版はnewlibが無く不可）
curl -L -o /tmp/arm.tar.xz https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi.tar.xz
mkdir -p ~/gcc-arm-none-eabi && tar -xf /tmp/arm.tar.xz -C ~/gcc-arm-none-eabi --strip-components=1
# SDK
git clone https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
git -C ~/pico-sdk submodule update --init lib/tinyusb
```

## コマンド

```bash
export PICO_SDK_PATH=~/pico-sdk
export PATH=~/gcc-arm-none-eabi/bin:$PATH
cd /Users/akihito/works/expresslrs_realtime_recorder
mkdir -p build
cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

## ビルド成功時の出力
- `build/` ディレクトリに `.uf2` ファイルが生成される
- このファイルをPicoにドラッグ&ドロップでflash可能
- 参考: Flash約40KB / RAM約3.3KB 使用

## トラブルシューティング
- `PICO_SDK_PATH` が未設定なら上記 export を実行
- `nosys.specs が無い`等のリンクエラーは brew formula 版ツールチェーン（newlib欠落）が原因。公式 tar.xz 版を使う
- CMakeエラーの場合は `build/` を削除して再実行
