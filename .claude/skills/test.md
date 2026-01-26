# /test - ユニットテストスキル

## 概要
プロジェクトのユニットテストを実行します。

## テスト対象
- CRSFプロトコルのパケット生成
- チェックサム計算
- チャンネル値の変換ロジック

## 実行手順

1. テスト用ビルドディレクトリを準備
2. テストをビルド
3. テストを実行

## コマンド

```bash
cd /Users/akihito/works/expresslrs_realtime_recorder
mkdir -p build_test
cd build_test
cmake -DBUILD_TESTS=ON ..
make -j$(sysctl -n hw.ncpu)
ctest --output-on-failure
```

## テストフレームワーク
- ホストPC上で実行可能なユニットテスト
- Pico固有のハードウェア依存部分はモック化

## テスト結果
- 全テスト成功: 緑色で "All tests passed"
- 失敗時: 失敗したテストケースと理由を表示
