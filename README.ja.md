# AMD Video Analysis Framework

[English](README.md)

AMD の APU で動く、GStreamer ベースの映像解析フレームワークです。カメラ映像や動画ファイルのデコード、物体検出などの推論、検出結果の描画、エンコードまでを GPU 上で処理し、CPU には検出結果などのメタデータだけを戻します。Ryzen AI 9 HX 370 / Radeon 890M で検証しています。

## デモ

<!-- NVR デモの動画をここに置く -->

複数のカメラを録画しながら検出結果を表示する NVR デモの内容と起動方法は、[NVR デモ](docs/guide/nvr-demo.ja.md) を参照してください。

## 特徴

- **GPU 上で完結する処理**: デコード、前処理、推論、描画、エンコードの間で、画素を CPU にコピーしません。
- **GStreamer の要素で組み立てる**: `gst-launch-1.0` のパイプラインで、物体検出、分類、インスタンス/セマンティックセグメンテーション、姿勢推定を使えます。
- **DL Streamer の要素を同梱**: トラッキング（`gvatrack`）、複数ストリームの束ね、メタデータの変換を、DL Streamer と同じ要素名で使えます。
- **結果の出力**: 検出結果を JSON Lines、MQTT、Kafka で送信できます。描画した映像はハードウェアエンコードして、ファイルに保存したり RTSP で配信したりできます。
- **C++ SDK**: RTSP カメラの受信、録画、録画の検索と再生を、GUI ライブラリに依存せずにアプリケーションへ組み込めます。
- **サンプル**: 16 カメラの NVR デモ、GPU で描画するリソースダッシュボード、動画ファイルを RTSP カメラとして配信するツール。

## 動作環境

| 項目 | 検証済みの環境 |
| --- | --- |
| ハードウェア | Ryzen AI 9 HX 370 / Radeon 890M（gfx1150） |
| OS | Ubuntu 24.04、カーネル 6.17.0-1032-oem 以降、Mesa 25.2.8 |
| 依存関係 | GStreamer 1.28.2、ROCm 7.2.0 / MIGraphX 2.15.0（ブートストラップがリポジトリ内にインストールします） |

## ドキュメント

ドキュメントは <https://anjn.github.io/amd-video-analysis-framework/ja/> で読めます。

- 初めて使う場合: [インストール](docs/guide/install.ja.md) → [クイックスタート](docs/guide/quickstart.ja.md)
- パイプラインの組み方: [パイプラインの作り方](docs/guide/pipelines.ja.md)
- アプリケーションへの組み込み: [SDK でアプリケーションに組み込む](docs/guide/sdk.ja.md)
- 設計、全プロパティ、検証結果: [開発者向け情報](docs/dev/framework.ja.md)

## ライセンス

このリポジトリのコードは [MIT ライセンス](LICENSE) です。同梱した外部のソース（DL Streamer の一部、sqlite-vec など）はそれぞれのライセンスに従います。一覧は [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) にあります。モデルの重みとサンプル画像は含めていません。利用する場合は、それぞれの利用条件を確認してください。
