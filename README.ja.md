<!-- source: README.md 6c5cb3f5919696afe43e3ef15eaea2dcef796991 -->
# AMD Video Analysis Framework

[English](README.md)

AMD APU 向けの GStreamer 映像解析フレームワークです。VA コーデック/VPE、HIP による画像処理、MIGraphX による推論、VA ハードウェアエンコードを使います。Intel DL Streamer の一部（`gvatrack`、`gvastreammux`、`gvametaconvert` など）を同梱しており、トラッキング、ストリームの mux/demux、メタデータの集約、相互運用はこれらの要素が担います。

```text
RTSP / file → VA decode → amddetect → gvatrack → amdclassify (ROI)
                       → amdpython → amdmetapublish → amdwatermark → amdencode → file / RTSP
```

YOLOv8 の検出、インスタンスセグメンテーション、姿勢推定、分類ロジット、セマンティックセグメンテーション、汎用の GPU テンソル結果に対応します。画像とマスクの処理は GPU 上で完結し、CPU に戻るのはサイズに上限のある結果メタデータだけです。共有バッチは 1/2/4/8/16 の固定サイズと、未充足バッチのタイムアウトに対応します。

Qt に依存しない `amdva_streaming` ライブラリは、RTSP の受信、セグメント単位の録画、容量上限付きの録画/解析カタログ、ハードウェア再生を提供します。インストール可能な
CMake SDK も含まれます。

アプリケーションはフレームワーク本体から分離し、[`examples/`](examples) に置いています。人物検索付きの 16 カメラ NVR、ハードウェアエンコードによるリソースダッシュボード、ファイルを RTSP で配信するテストサーバー、SDK のサンプルプログラム、`gst-launch` のレシピがあります。いずれも公開ヘッダーと `AMDVA::` ターゲットだけを使います。

## ドキュメント

ドキュメントは <https://anjn.github.io/amd-video-analysis-framework/ja/> で閲覧できます。ローカルでプレビューするには、`pip install -r requirements-docs.txt` を実行してから
`python3 scripts/docs_site.py && mkdocs serve` を実行します。

| トピック | |
| --- | --- |
| DL Streamer からの移行 | [要素とプロパティの対応表、モデル、サンプル](docs/migration-from-dlstreamer.ja.md) |
| DeepStream からの移行 | [要素の対応表、`config_infer` の対応、メタデータ](docs/migration-from-deepstream.ja.md) |
| 要素 | [リファレンス: caps、プロパティ、既定値](docs/elements.ja.md) |
| モデル | [model-info JSON とコンパイル](docs/model-info.ja.md) |
| メタデータ | [JSON スキーマ、`amdpython`、結果の送信](docs/metadata-and-python.ja.md) |
| エンコード | [ハードウェアエンコード、録画、RTSP 配信](docs/encode-and-serve.ja.md) |
| SDK | [ライブラリ、CMake パッケージ、サンプル、テスト](docs/sdk-guide.ja.md) |
| gst-launch のレシピ | [examples/pipelines](examples/pipelines/README.ja.md) |

[フレームワークの対応機能とサンプル](docs/framework.ja.md)、[メモリに関する契約](docs/architecture.ja.md)、[NVR の使い方](examples/nvr/README.ja.md)、[AI 統計と 16 カメラでの評価](examples/nvr/docs/nvr-ai-performance.ja.md)、[検証](docs/validation.ja.md)、[デコード/表示のテレメトリ](docs/telemetry.ja.md) も参照してください。AMD の要素は独自の model-info 形式を使います。上流 DL Streamer のモデルアダプターや gva 推論バックエンドをすべて実装しているわけではありません。従来の `amdpreproc ! amdmigraphx` パイプラインも引き続き使えます。

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

依存関係は [dependencies.lock.json](dependencies.lock.json) で SHA256 ハッシュとともに固定しています。内訳は、DL Streamer の analytics 拡張パッチ（`patches/`）を当てた GStreamer 1.28.2、ROCm 7.2.0 / MIGraphX 2.15.0、Paho MQTT、librdkafka、組み込み CPython です。最後の 3 つは省略可能です（`AMDVA_WITH_MQTT`、`AMDVA_WITH_KAFKA`、`AMDVA_WITH_PYTHON`。[SDK ガイド](docs/sdk-guide.ja.md#build-and-install) を参照）。DL Streamer v2026.2.0 のソースは `third_party/dlstreamer`（MIT）に同梱しているため、クローンも OpenVINO も不要です。推論には MIGraphX を使います。`python3 scripts/bootstrap.py --check` で、キャッシュ済みの依存関係のダウンロードを検証できます。パイプラインの実行中に、読み込まれているライブラリを再インストールしたり再ビルドしたりしないでください。

## モデルの準備と実行

入力が `[1,3,640,640]` の RGB、出力が `[1,84,8400]` の固定形状の YOLOv8 COCO ONNX を使います。モデルのメタデータは付属の[サンプル](examples/yolov8n.json) に従います。80 行のラベルファイルはその JSON と同じ場所にあります。背景となった既存プロジェクトのモデルとラベルファイルは、そのまま使えます。

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

長時間試験では、RSS、FD、確保済み GPU メモリを定期的に記録し、ウォームアップ後の増加が上限内に収まるかを確認します。DL Streamer との連携とコピー監査の成功も合格条件です。合格基準と再現手順は[検証](docs/validation.ja.md) を参照してください。

映像の録画では、圧縮された RTSP パケットをそのまま保存します。このフレームワークは
NPU の推論バックエンドを提供しません。[リソースダッシュボードのサンプル](examples/resource-dashboard/README.ja.md) は、GPU で描画した監視グラフィックスをハードウェア H.264 エンコードで配信します。

## ライセンス

新規のコードは MIT ライセンスです。外部のソースはそれぞれ個別のライセンスに従います。[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) を参照してください。モデルとサンプル画像の利用条件は、再配布の前に別途確認する必要があります。

## サンプル

```text
include/, src/, tools/, tests/   framework libraries, plugins, tools and contract tests
examples/nvr/                    NVR sample: backend/, app/ (Qt), headless service, tools, docs
examples/resource-dashboard/     GPU-rendered RTSP resource dashboard
examples/rtsp-files/             file-to-RTSP test server
examples/sdk/, examples/pipelines/  small SDK programs and gst-launch recipes
```

`-DAMDVA_BUILD_EXAMPLES=OFF` を指定するとフレームワークだけをビルドします。各サンプルのディレクトリは、インストール済みの SDK を使って単独で configure することもできます。

NVR サンプルは、動的に登録した RTSP カメラを受信して録画し、選択した最大 16 個の
H.264/H.265 チャンネルを VA/VPE/EGL で表示して、YOLOv8n の GPU バッチ検出の結果をオーバーレイします。自動再接続と、容量上限に基づく管理対象 MKV 録画（書き込み完了分）の削除に対応します。[NVR のセットアップ、アーキテクチャ、検証](examples/nvr/README.ja.md)と [設定例](examples/nvr/config/nvr.json) を参照してください。省略可能なバックグラウンド AI は、非表示のソースから単独でデコードできるキーフレームだけを、設定可能な間隔（既定は 5 秒）でデコードし、上限付きの 4 つの AI スロットで処理します。

デスクトップサンプルには、固定のライブレイアウト、カメラ管理、録画検索、ハードウェア再生が含まれます。[VMS GUI ガイド](examples/nvr/docs/vms.ja.md) を参照してください。

[リソースダッシュボードのプロトタイプ](examples/resource-dashboard/PROTOTYPE.ja.md) は、CPU、GPU、DRAM の監視を単体の HTML でプレビューするものです。値は模擬値で、1080p の映像合成を含みます。[ネイティブ RTSP サンプル](examples/resource-dashboard/README.ja.md) は、実際のメトリクスを収集し、GPU 描画とハードウェア H.264 エンコードで合成映像を配信します。

動画ファイルを繰り返し再生できる RTSP カメラ入力として使う方法は、[ファイルを RTSP で配信するツール](examples/rtsp-files/README.ja.md) を参照してください。
