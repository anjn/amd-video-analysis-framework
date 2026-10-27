<!-- source: docs/framework.md 323047e9446687bc93a1805b4e783b78c53cd7ec -->
# 映像解析フレームワーク

AMD フレームワークは、映像解析パイプラインの主要な処理を扱います。デコード、画像/ROI の前処理、推論、検出/分類/セグメンテーション/姿勢推定の後処理、トラッキング、メタデータ、描画、Python コールバック、結果の送信、ハードウェアエンコードです。トラッキング、メタデータ変換、ストリームの mux には DL Streamer の一部（`third_party/dlstreamer`、MIT）を同梱して使います。AMD の推論を `gvadetect` という名前に置き換えたり、上流の OpenVINO 推論に
MIGraphX バックエンドを追加したりはしていません。既存の DL Streamer の `model-proc` ファイルや上流のモデルアダプターは、そのままでは使えません。DL Streamer や DeepStream から移行する開発者は、まず [DL Streamer からの移行ガイド](migration-from-dlstreamer.ja.md) または
[DeepStream からの移行ガイド](migration-from-deepstream.ja.md) を読んでください。音声、生成 AI、3D センサーフュージョン、NPU での実行は、この実装の対象外です。

## 構成要素

| ライブラリ | 公開ヘッダー | 役割 |
| --- | --- | --- |
| `amdva_runtime` | `runtime.hpp`、`preprocess.hpp`、`telemetry.hpp` | VA/HIP のインポート、VPE 処理、GPU での画像パッキング、画像の逆幾何変換 |
| `amdva_inference` | `model.hpp`、`batching.hpp` | 名前付きの固定形状 GPU テンソル入出力、モデルのマニフェスト、タスクアダプター、上限付きの共有バッチ |
| `amdva_gst_support` | `tensor_meta.hpp`、`video_preprocess.hpp`、`tracking.hpp`、`overlay.hpp`、`publishing.hpp`、`gst_telemetry.hpp` | GstAnalytics との相互運用、GPU ペイロードの所有権管理、映像の前処理、画像を使わないトラッキング、描画と出力 |
| `amdva_media` | `vui.hpp`、`rtsp_server.hpp`、`egl_import.hpp` | BT.709 の H.264 VUI シグナリング、appsrc 用 RTSP サーバーのラッパー、Qt に依存しない VA から EGL への DMA-BUF テクスチャインポート |
| `amdva_streaming` | `streaming/*.hpp` | RTSP のアクセスユニット単位の受信、圧縮データのままのセグメント録画、容量上限付きの SQLite 録画/解析カタログ、キーフレームの検査、ハードウェア再生、GPU プレビュー用サーフェス |
| `amdva_monitor` | `resource_monitor.hpp` | CPU/GPU/メモリ/電力の計測値と DRM エンジンの使用率 |

`amdva_streaming` は Qt に依存しません。提供するのはアプリケーションそのものではなく部品です。カメラのスケジューリング、AI の受け入れ制御、検索のポリシーはアプリケーション側で決めます。[`examples/`](../examples) 以下のアプリケーションは、公開ヘッダーと `AMDVA::` ターゲットだけを使ってビルドしています。

| サンプル | 内容 |
| --- | --- |
| [`examples/nvr`](../examples/nvr/README.ja.md) | 16 カメラ NVR。`amdva_streaming` の上に構築した Qt に依存しないコントローラー（`amdva_nvr_backend`）、人物検索、ヘッドレスの `amdva-streams` サービス、Qt デスクトップ |
| [`examples/resource-dashboard`](../examples/resource-dashboard/README.ja.md) | `amdva_monitor` を使い、GPU で描画してハードウェアエンコードする RTSP ダッシュボード |
| [`examples/rtsp-files`](../examples/rtsp-files/README.ja.md) | ファイルを RTSP で配信するテストサーバー |
| [`examples/sdk`](../examples/sdk) | トランスコード、テレメトリ、録画映像 API の小さなプログラム |
| [`examples/pipelines`](../examples/pipelines/README.ja.md) | `gst-launch` のレシピ |

NVR サンプルの画像パッカー、バッチ実行器、トラッキング、結果の送信には、プラグインと同じフレームワークの実装を使っています。

## 要素と対応するタスク

| 要素 | 対応する処理 |
| --- | --- |
| `amddetect` | YOLOv8 の検出 `[B,4+C,N]`、`[B,M,H,W]` のプロトタイプを使うインスタンスセグメンテーション、K x 3 のキーポイントチャンネルを使う姿勢推定 |
| `amdclassify` | 分類ロジット `[B,C]`、softmax/top-k。既定は `inference-region=roi-list` |
| `amdinference` | model-info で選んだタスクをフレーム全体に対して実行。セマンティックロジット `[B,C,H,W]` や `task_type=raw` の GPU テンソルも含みます |
| `amdwatermark` | GPU でのボックス、クラスラベル、トラッキング ID、マスク、COCO-17 骨格の描画。入力は NV12 VAMemory、出力は BGRA DMA-BUF |
| `amdpython` | メタデータだけを扱う `callback(frame_dict)` 拡張。ピクセルや GPU アドレスは公開しません |
| `amdmetapublish` | 上限付きの非同期な JSON lines、MQTT QoS1、Kafka への送信 |
| `amdencode` | VAMemory または BGRA DMA-BUF のフレームを対象とする VA ハードウェア H.264/H.265/AV1 エンコーダーの bin。[エンコードと配信](encode-and-serve.ja.md) を参照 |
| `amdpreproc`、`amdmigraphx` | 互換性のために残している、既存の 2 要素構成の YOLOv8 インターフェース |

同梱の DL Streamer の一部には、`gvatrack`、`gvametaconvert`、`gvametapublish`（file、MQTT、Kafka モジュール）、`gvastreammux`、`gvastreamdemux`、`gvametaaggregate` が含まれます。上流のそれ以外の要素（`gvadetect`、`gvaclassify`、`gvainference`、`gvawatermark`、`gvapython`、`gvaattachroi`、`gvaanalytics`、`gvafpscounter`、`gvafpsthrottle`）は含まれないため、`amd*` 要素を使ってください。すべてのプロパティは [要素リファレンス](elements.ja.md) を参照してください。

画像を使わない `gvatrack` は、画像を CPU にマップしません。上流の画像ベースのトラッカーと
watermark の処理経路は AMD の GPU メモリに関する契約を満たさないため、代替としては有効にしていません。選択中のソースを扱う NVR は継続的にトラッキングして ID を描画します。バックグラウンドのキーフレームの結果は個別の観測であり、トラッキング ID はありません。

## 代表的なモデルの準備

エクスポーターは、別に用意した CPU 環境で実行します。torchvision の
ResNet18 ImageNet V1 の重み、Ultralytics v8.3.0 の YOLOv8n セグメンテーション/姿勢推定の重み、リビジョンを固定した NVIDIA の SegFormer-B0 ADE20K モデルを使います。エクスポート時には、重みとソースの識別情報、ONNX の SHA256 ハッシュを記録します。ここでの ResNet18 は直接リサイズと
ImageNet の正規化を使います。torchvision の評価手順（リサイズ後に中央を切り出す方式）とは異なります。

```bash
source scripts/env.sh
python3 scripts/setup_model_export.py
.deps/model-export/bin/python scripts/export_analytics_models.py artifacts/models
for name in resnet18 yolov8n-seg yolov8n-pose segformer-b0; do
  build/src/gst/amdva-compile-model artifacts/models/$name.onnx \
    artifacts/models/$name.mxr --model-info=artifacts/models/$name.json
done
```

モデルの重みは外部の成果物であり、このリポジトリの MIT ライセンスには含まれません。想定する配備に合わせて、[torchvision の重みに関するドキュメント](https://docs.pytorch.org/vision/stable/models/generated/torchvision.models.resnet18.html)、[Ultralytics のライセンス](https://www.ultralytics.com/license)、[NVIDIA のモデルカード/ライセンス](https://huggingface.co/nvidia/segformer-b0-finetuned-ade-512-512)を確認してください。エクスポーターは、変化する `main` ブランチを解決するのではなく、SegFormer リポジトリのリビジョンを記録します。

model-info には、画像の前処理と出力アダプターを記述します。

```json
{
  "task_type": "classification",
  "adapter": "classification",
  "input": {
    "name": "images", "shape": [1, 3, 224, 224],
    "format": "NCHW", "channel_order": "RGB", "letterbox": false,
    "scale": 0.00392156862745098,
    "mean": [0.485, 0.456, 0.406], "std": [0.229, 0.224, 0.225]
  },
  "output": {"num_classes": 1000, "top_k": 5},
  "labels": "resnet18.txt"
}
```

正規化は `(pixel * scale - mean) / std` です。RGB の既定値は、スケール
1/255、mean 0、std 1、パディング 114 です。ラベルのパスは model-info からの相対パスです。対応するタスク名は `detection`、`classification`、`instance-segmentation`、`semantic-segmentation`、`pose`、`raw` です。YOLO アダプターは、デコード済みの中心/サイズ形式のボックスとクラス確率を想定しており、独立した objectness チャンネルは扱いません。YOLOv5、SSD、任意の model-proc 変換、動的なテンソル形状には、これらのアダプターは対応していません。

コンパイラーは `--batch=1|2|4|8|16`、`--precision=fp16|fp32`、`--model-info=FILE`、繰り返し指定できる `--input-shape=NAME:D1,D2,...` を受け付けます。C++ の
`ModelExecutor` は名前付きの固定形状入力を複数受け付け、所有権付きの GPU 出力を返します。外部の型として対応するのは float32/float16/int32/int64/uint8 です。画像を自動で扱う要素とバッチ実行器は、意図的に float32 NCHW の画像入力 1 つだけを要求します。複数入力のテンソルは C++ API から渡してください。後処理器は外部出力が float32 であることを要求します。FP16 でコンパイルすると内部演算が変わりますが、外部の型は元のまま保たれます。

## GStreamer パイプライン

ランタイム環境を読み込んだ後、`VIDEO`、`DETECT`、`INFO`、`MODELS` にローカルのパスを設定します。検出器のモデルと model-info は、NVR で使うものと同じ成果物です。新しいタスク要素は内部で前処理を行うため、その前に従来の `amdpreproc` を挿入しないでください。

```bash
gst-launch-1.0 -e filesrc location="$VIDEO" ! h264parse ! vah264dec ! \
  'video/x-raw(memory:VAMemory),format=NV12' ! \
  amddetect model="$DETECT" model-info="$INFO" inference-interval=5 ! \
  gvatrack tracking-type=short-term-imageless ! \
  amdclassify model="$MODELS/resnet18.mxr" model-info="$MODELS/resnet18.json" ! \
  amdpython module="$PWD/examples/metadata_callback.py" ! \
  amdmetapublish method=file file-path=events.jsonl ! \
  amdwatermark ! fakesink sync=false
```

分類/検出の連結部分は、次のいずれかのタスク要素に置き換えられます。

```text
amddetect model=.../yolov8n-seg.mxr model-info=.../yolov8n-seg.json
amddetect model=.../yolov8n-pose.mxr model-info=.../yolov8n-pose.json
amdinference model=.../segformer-b0.mxr model-info=.../segformer-b0.json
```

分類を単独で使う場合は `inference-region=full-frame` を設定します。ROI モードでは、標準の GstAnalytics のオブジェクトボックスを最大 300 個読み取り、VPE で切り出して、分類結果を親オブジェクトに関連付けます。ROI の座標は NV12 の色差の境界に合わせて外側に広げ、実際の切り出し範囲を付加情報に記録します。セグメンテーションのマスクは GPU テンソルのまま保持し、JSON にはマスクのピクセルではなく格納形式と形状を記述します。汎用の raw 推論でも、同様に出力テンソルを GPU 上に保持します。

推論のプロパティはすべて NULL/READY 状態でのみ変更できます。既定値は、`device` が 0、`batch-size` が 1、`batch-timeout` が 20 ms、`inference-interval` が 1、`confidence-threshold` が .25、`nms-threshold` が .45 です。厳格な GPU モードは必須です。開始前に、選んだバッチサイズでモデルをコンパイルしてください。

## 共有バッチと複数ストリーム {#shared-batches-and-multiple-streams}

空でない同じ `model-instance-id` を指定すると、モデルのパス、メタデータ、デバイス、バッチサイズ、キューの上限、しきい値がすべて同じ要素の間で実行器を共有します。同じ ID で設定が異なる場合は拒否します。ワーカーは 1 つのバッチにソースごとに最大 1 件のリクエストを受け入れ、最大 `batch-timeout` まで待ち、未充足のバッチを GPU 上でパディングします。GPU から GPU への組み立ては許可しており、CPU 上での画像やテンソルの中継は発生しません。ROI のリクエストは個別の子ソースとして扱います。キューのあふれは無制限のバッファーリングではなく、明示的なエラーになります。ソースの世代が変わると、キュー内のジョブを取り消し、実行中の古い結果を破棄します。すでに実行中の GPU カーネルは、そのメモリを解放する前に完了します。

複数のパイプライン分岐を並行して実行する場合は、各推論要素に同じ ID とコンパイル済みのバッチサイズを指定します。解像度が異なる場合は、`gvastreammux
output-mode=container` で各ソースの元の caps とメモリを保持します。AMD の推論要素は、各子ソースをそれぞれの VA コンテキストで前処理し、GPU テンソルをバッチにまとめて、結果を `gvastreamdemux` に返します。

```text
source0 ! decode ! video/x-raw(memory:VAMemory),format=NV12 ! mux.sink_0
source1 ! decode ! video/x-raw(memory:VAMemory),format=NV12 ! mux.sink_1
gvastreammux name=mux output-mode=container !
  amddetect model=... model-info=... batch-size=4 ! gvastreamdemux name=demux
demux.src_0 ! queue ! gvatrack tracking-type=short-term-imageless ! amdwatermark ! sink0
demux.src_1 ! queue ! gvatrack tracking-type=short-term-imageless ! amdwatermark ! sink1
```

シンクのプリロールやバックプレッシャーですべてのソースが止まらないように、demux の各分岐には個別の queue が必要です。採用した mux パッチは、コンテナー内で元の stream-start の識別情報を保持します。AMD の要素が受け付けるのは、トラッキング対象のソースインデックスが最大
256 個までの映像だけのコンテナーで、任意の LiDAR や音声のオブジェクトは扱いません。

## メタデータ、コールバック、出力

標準の GstAnalytics には、オブジェクト、分類、トラッキングの関連、キーポイントのグループが含まれます。`AmdAnalysisMeta` は、GPU のマスク/raw テンソル、逆幾何変換、ソース/世代/シーケンス/PTS の識別情報を追加します。コピー時には、変更可能な付加情報のリストを複製し、共有の GPU メモリは保持し続けます。バッファーは、それを生成したモデルやパイプラインより長く存続することがあります。`gvametaaggregate` は採用したパッチにより、分岐を統合する際に
GPU ペイロードを保持し、ROI の親 ID を付け替えます。GPU テンソルのメタデータを領域単位でコピーしようとすると、誤って再解釈するのではなく拒否します。

`amdpython` は既定で `process(frame_dict)` を呼び出します。`None` または `True` を返すとそのまま通過させ、`False` を返すとフレームを破棄し、JSON にシリアライズできる dict を返すと独自のメタデータとして付加します。例外はバスのエラーになります。独自 JSON の上限は 64 KiB です。コールバックはストリーミングスレッド上で Python の GIL を取得して実行するため、遅いコールバックはスループットを下げます。コールバックでは、簡潔な形式の検出/クラス/キーポイントとテンソルの形状を確認できます。ピクセルへのアクセスとデバイスポインターは意図的に提供していません。

`amdmetapublish method=mqtt address=tcp://127.0.0.1:1883 topic=vision/events`
と `method=kafka address=127.0.0.1:9092 topic=vision-events` は、NVR と同じ送信器を使います。既定値は `none` です。ファイルへの送信では JSON lines を追記します。SDK の `PublicationOptions` はレコード数とバイト数の上限（既定は 256 と
4 MiB）を設定します。1 レコードの上限は 64 KiB です。キューが満杯になると待機中の最も古いレコードを破棄し、ブローカーの障害は状態に反映し、再試行は映像処理とは独立して行います。MQTT は QoS1 を使い、Kafka は配信コールバックを使います。これらは上限付きのベストエフォートの出力であり、永続的なイベント保存や exactly-once のイベント保存ではありません。MQTT/Kafka のクライアントにも上限付きの内部キューがあります。要素の実行中は `delivered`、`dropped`、`errors` を読み取るか、SDK の `PublicationStatus` と `flush(timeout_ms)` を使ってください。

JSON スキーマ 1 には、ソース、世代、シーケンス、PTS、バックグラウンドフラグ、ボックス/クラス/トラッキング/キーポイントを持つオブジェクト、フレーム全体の分類、データ型、デバイス、バッチインデックスを持つ GPU テンソル記述子、省略可能な独自メタデータが含まれます。画像バッファー、マスク、GPU ポインターはシリアライズしません。ソースの URL は自動では追加しません。ソースの識別情報はアプリケーションが決めます。

## ヘッドレスビルドと SDK

```bash
source scripts/env.sh
cmake -S . -B build-headless -DAMDVA_BUILD_NVR_GUI=OFF \
  -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT"
cmake --build build-headless -j6
build-headless/examples/nvr/amdva-streams examples/nvr/config/nvr.json 60
cmake --install build-headless --prefix /path/to/amdva-sdk
```

`-DAMDVA_BUILD_NVR_GUI=OFF` を指定すると、NVR のバックエンドとヘッドレスの
`amdva-streams` サービスはビルドしたまま、Qt は不要になります。`-DAMDVA_BUILD_EXAMPLES=OFF` を指定するとフレームワークだけをビルドします。`amdva-streams` はソースを受信/録画し、選択中のソースとバックグラウンドの解析を行います。EGL プレビューの確保やディスプレイサーバーは必要ありません。実行時間に 0 を指定するか省略すると、SIGINT/SIGTERM を受けるまで実行します。

外部の CMake プロジェクトは `find_package(AMDVideoAnalysis REQUIRED)` を使い、`AMDVA::amdva_streaming`、`AMDVA::amdva_gst_support`、`AMDVA::amdva_inference`、`AMDVA::amdva_media`、`AMDVA::amdva_monitor`、`AMDVA::amdva_runtime` のいずれかをリンクします。ソースツリーにも同じターゲットがあるため、`examples/` 以下の各ディレクトリは、インストール済みの SDK を使って単独で configure することもできます（[`examples/sdk/CMakeLists.txt`](../examples/sdk/CMakeLists.txt) を参照）。インストールしたパッケージには、互換性のあるホストの VA/GStreamer/ROCm スタックが必要です。可搬なドライバーランタイムは同梱していません。互換性のあるインストール環境で HIP のヘッダーを探す場合は `AMDVA_ROCM_ROOT` を設定してください。

## デコードと表示のテレメトリ

Qt に依存しないテレメトリ収集器と GStreamer アダプターは、ローカルでの RTP 受信からデコーダー出力までと、レンダラーの通知までを計測します。移動区間のパーセンタイル、フレームレート、更新間隔の空き、既知のローカル破棄カウンターを集計します。NVR とヘッドレスのコマンドは、統計を AI の結果 JSON とは別に送信します。遅延や FPS の合格基準、自動制御は設けていません。[計測区間、SDK アダプター、検証](telemetry.ja.md) を参照してください。


録画の検索、保護付きの再生ハンドル、Qt に依存しない録画プレーヤー、録画の解析結果ストアは `amdva_streaming` に含まれます。アプリケーションは
`Catalog::extend` を使って、カタログデータベースに独自のテーブルを持てます（NVR の人物検索インデックスはその一例です）。[VMS のインターフェースと移行](../examples/nvr/docs/vms.ja.md#framework-interfaces-and-migration)と [SDK の再生サンプル](../examples/sdk/playback.cpp) を参照してください。

Qt に依存しない `amdva_monitor` ライブラリは、`amdva/resource_monitor.hpp` を通じて、値が取得できない場合もある CPU/GPU/メモリ/電力の計測値と、観測可能な DRM エンジンの使用率を提供します。[リソースダッシュボードのサンプル](../examples/resource-dashboard/README.ja.md) は、NVR、解析プラグイン、モデルの読み込みとは独立してこのライブラリを使います。
