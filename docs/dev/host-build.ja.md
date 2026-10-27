<!-- source: docs/dev/host-build.md c13a10ae80b50c719bbdea28bb80f07dc03a7c14 -->
# ホストでのビルドと基準パイプラインの実行

短い手順はユーザーガイド（[インストール](../guide/install.ja.md)、[クイックスタート](../guide/quickstart.ja.md)）にあります。このページには、ホストに関する詳細、依存関係のロック、コピー監査付きの基準実行ツールの使い方を残しています。

## ホストでのビルド

検証済みのハードウェアは Ryzen AI 9 HX 370 / Radeon 890M（**gfx1150**）、VPE 6.1.0 です。元のログイン環境では gfx1151 が強制されていましたが、`scripts/env.sh` がこの設定を解除します。ソフトウェアの基準環境は Ubuntu 24.04、カーネル 6.17.0-1032-oem、Mesa
25.2.8 です。`/dev/dri/renderD128` と `/dev/kfd` を通じた GPU デバイスへのアクセスが必要です。

ホストの前提条件は、Python 3.12+、`uv`、C/C++ コンパイラー、CMake 3.22+、pkg-config、git、dpkg-deb、apt-get、m4、GLib の開発ファイル（`glib-2.0 >= 2.64`）です。テスト用素材の作成には libx264 付きの FFmpeg を使います。ブートストラップは sudo を使わず、システムのライブラリも置き換えずに `.deps/` へインストールします。数 GB の ROCm
パッケージをダウンロードし、展開後のライブラリにはさらに多くのディスク容量が必要です。

```bash
python3 scripts/bootstrap.py --jobs 8
source scripts/env.sh
ctest --test-dir build --output-on-failure
build/amdva-probe
build/src/gst/amdva-validate-preproc
```

依存関係は [dependencies.lock.json](../../dependencies.lock.json) で SHA256 ハッシュとともに固定しています。内訳は、DL Streamer の analytics 拡張パッチ（`patches/`）を当てた GStreamer 1.28.2、ROCm 7.2.0 / MIGraphX 2.15.0、Paho MQTT、librdkafka、組み込み CPython です。最後の 3 つは省略可能です（`AMDVA_WITH_MQTT`、`AMDVA_WITH_KAFKA`、`AMDVA_WITH_PYTHON`。[SDK ガイド](sdk-guide.ja.md#build-and-install) を参照）。DL Streamer v2026.2.0 のソースは `third_party/dlstreamer`（MIT）に同梱しているため、クローンも OpenVINO も不要です。推論には MIGraphX を使います。`python3 scripts/bootstrap.py --check` で、キャッシュ済みの依存関係のダウンロードを検証できます。パイプラインの実行中に、読み込まれているライブラリを再インストールしたり再ビルドしたりしないでください。

## モデルの準備と実行

入力が `[1,3,640,640]` の RGB、出力が `[1,84,8400]` の固定形状の YOLOv8 COCO ONNX を使います。モデルのメタデータは付属の[サンプル](../../examples/yolov8n.json) に従います。80 行のラベルファイルはその JSON と同じ場所にあります。背景となった既存プロジェクトのモデルとラベルファイルは、そのまま使えます。

```bash
source scripts/env.sh
mkdir -p artifacts
build/src/gst/amdva-compile-model /path/yolov8n.onnx artifacts/yolov8n.mxr \
  > artifacts/model-compile.log 2>&1
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/src/gst/amdva-run /path/input.h264 artifacts/yolov8n.mxr \
  /path/yolov8n.json --events=artifacts/events.jsonl
```

モデルの準備では、内部演算に FP16 を使い、`offload_copy=false` を指定します。MXR と同じ場所に SHA256 と GPU 識別情報のマニフェストを書き出し、実行時にはマニフェストがない場合、一致しない場合、互換性がない場合にモデルを拒否します。既存の MXR ファイルは再コンパイルが必要です。モデルのコンパイルは、実行時のパイプライン制御とは分かれています。

`amdva-run` は、フレーム数、検出数、処理遅延の P95/P99、元のメモリと PTS の確認結果、コピー監査の結果を出力します。`--amd-only` を指定すると、診断のために DL Streamer を使わずに実行します。コピー監査ライブラリを使わない場合、レポートのゼロコピー検証は明示的に false のままになります。`--duration=SECONDS` は 30fps の H.264 エレメンタリストリームのテスト用素材を実時間のペースで繰り返し再生します。SIGINT/SIGTERM を受けると EOS を要求し、パイプラインに残ったデータを処理してから終了します。

```bash
python3 tools/soak.py /path/input.h264 artifacts/yolov8n.mxr /path/yolov8n.json \
  --seconds=3600 --output=artifacts/soak
```

長時間試験では、RSS、FD、確保済み GPU メモリを定期的に記録し、ウォームアップ後の増加が上限内に収まるかを確認します。DL Streamer との連携とコピー監査の成功も合格条件です。合格基準と再現手順は[検証](validation.ja.md) を参照してください。

映像の録画では、圧縮された RTSP パケットをそのまま保存します。このフレームワークは
NPU の推論バックエンドを提供しません。[リソースダッシュボードのサンプル](resource-dashboard/README.ja.md) は、GPU で描画した監視グラフィックスをハードウェア H.264 エンコードで配信します。
