# NVR デモ

[English](README.md)

登録したすべての RTSP カメラを録画しながら、最大 16 台をデスクトップに表示し、検出結果とトラッキング ID を重ねて描画するネットワークビデオレコーダーのサンプルです。録画はタイムラインから再生でき、人物検索も試せます。起動手順と画面の説明は [NVR デモのガイド](../../docs/guide/nvr-demo.ja.md) にあります。

| ディレクトリ | 内容 |
| --- | --- |
| `backend/` | Qt に依存しないバックエンド（カメラの制御、録画、AI のスケジューリング、人物検索） |
| `app/` | Qt のデスクトップアプリケーション `amdva-nvr` |
| `headless.cpp` | 画面を使わずに動かす `amdva-streams` |
| `config/` | 設定ファイルの例 |
| `tools/`、`tests/`、`scripts/`、`benchmarks/` | 検証プログラム、テスト、モデルの準備とベンチマーク |

ビルドはフレームワークと一緒に行われます。Qt を使わない場合は `-DAMDVA_BUILD_NVR_GUI=OFF` を指定すると、`amdva-streams` だけをビルドします。設定項目、処理の構成、検証結果は [NVR の開発者向け情報](../../docs/dev/nvr/README.ja.md) を参照してください。
