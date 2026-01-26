# /build - Pico SDK ビルドスキル

## 概要
Raspberry Pi Pico用ファームウェアをビルドします。

## 実行手順

1. buildディレクトリが存在しない場合は作成
2. CMakeでビルドシステムを生成
3. makeでビルドを実行

## コマンド

```bash
cd /Users/akihito/works/expresslrs_realtime_recorder
mkdir -p build
cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

## ビルド成功時の出力
- `build/` ディレクトリに `.uf2` ファイルが生成される
- このファイルをPicoにドラッグ&ドロップでflash可能

## トラブルシューティング
- `PICO_SDK_PATH` が設定されていない場合は環境変数を確認
- CMakeエラーの場合は `build/` を削除して再実行
