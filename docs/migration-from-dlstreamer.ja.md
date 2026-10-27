<!-- source: docs/migration-from-dlstreamer.md 8dbfdba3d5ca9ca0a9d3c0947adcb8fd4d3e0021 -->
# Intel DL Streamer からの移行

このガイドでは、DL Streamer のパイプラインをこのフレームワークに対応付けます。パイプラインの構造は同じです（GStreamer の要素、GstAnalytics のメタデータ、トラッキング/変換/送信用の `gva*`）。変わるのは推論とメモリモデルです。推論は MIGraphX を通して AMD GPU 上で実行し、メモリモデルではデコーダーからエンコーダーまでフレームを GPU 上に保持します。

## 共通点

- 通常の GStreamer です。`gst-launch-1.0`、`gst_parse_launch`、bin、caps ネゴシエーションをそのまま使えます。
- メタデータは標準の `GstAnalytics` リレーションメタデータ（物体検出、分類、トラッキング、キーポイント/グループのメタデータ）として格納するため、下流の要素や独自のプローブから同じ方法で読み取れます。
- `gvatrack`、`gvametaconvert`、`gvametapublish`、`gvastreammux`、`gvastreamdemux`、`gvametaaggregate` は DL Streamer v2026.2.0 のソースを
  `third_party/dlstreamer` に同梱したもので、既定でビルドします（`AMDVA_BUILD_GVA_COMPAT`）。要素名とプロパティ名は変わりません。
- `inference-region`、`model-instance-id`、`batch-size`、`inference-interval`、`batch-timeout` のようなプロパティは、DL Streamer と同じ意味を持ちます。

## 要素の対応表

| DL Streamer | このフレームワーク | 備考 |
| --- | --- | --- |
| `gvadetect` | `amddetect` | YOLOv8 系（検出、インスタンスセグメンテーション、姿勢推定）。OpenVINO IR と `model-proc` には対応しません。 |
| `gvaclassify` | `amdclassify` | 既定値は `inference-region=roi-list` で、上流の検出器が出力したボックスを分類します。 |
| `gvainference` | `amdinference` | フレーム全体が対象です。セマンティックセグメンテーションと生の GPU テンソル（`task_type=raw`）にも対応します。 |
| `gvatrack` | `gvatrack`（同梱） | `tracking-type=short-term-imageless` または `zero-term-imageless` を使ってください。画像ベースのトラッカーはフレームを CPU にマップするため、有効にしていません。 |
| `gvawatermark` | `amdwatermark` | GPU のオーバーレイです。入力は VAMemory NV12、出力は BGRA DMA-BUF です。圧縮出力を得るには後段に `amdencode` を置きます。 |
| `gvapython` | `amdpython` | メタデータだけを受け取る `process(frame_dict)` コールバックです。画素や `VideoFrame` にはアクセスできません。 |
| `gvametaconvert` | `gvametaconvert`（同梱）、または `amdpython`/`amdmetapublish` の JSON | |
| `gvametapublish` | `gvametapublish`（同梱）または `amdmetapublish` | `amdmetapublish` は上限付きで、映像を止めることはありません。[メタデータ](metadata-and-python.ja.md)を参照してください。 |
| `gvastreammux` / `gvastreamdemux` | 同じ（同梱） | `output-mode=container` では、ソースごとの caps とメモリをそのまま保持します。 |
| `gvametaaggregate` | 同じ（同梱） | AMD の GPU ペイロードを統合するようにパッチを当てています（`amd-analytics-merge`）。 |
| `gvafpscounter`、`gvafpsthrottle`、`gvaattachroi`、`gvaanalytics` | 含まれません | `identity`/`fpsdisplaysink` を使うか、メタデータを扱うアプリケーションのコードで対応してください。 |
| `vah264enc`（GStreamer） | `amdencode` | BGRA DMA-BUF の入力と BT.709 の通知を追加します。[エンコードと配信](encode-and-serve.ja.md)を参照してください。 |

## プロパティ

### `gvadetect` / `gvaclassify` / `gvainference`

| DL Streamer のプロパティ | 対応する `amd*` のプロパティ |
| --- | --- |
| `model`（OpenVINO IR `.xml`） | `model`（`amdva-compile-model` でコンパイルした MIGraphX `.mxr`） |
| `model-proc` | `model-info`（JSON。[model-info](model-info.ja.md) を参照） |
| `device=GPU` / `CPU` / `NPU` | `device` は HIP デバイス番号です（既定値 0）。CPU や NPU へのフォールバックはありません |
| `batch-size` | `batch-size`（コンパイル時のバッチサイズと一致させる必要があります。1-16） |
| `inference-interval` | `inference-interval` |
| `threshold` | `confidence-threshold`（検出）と `nms-threshold` |
| `inference-region` | `inference-region`（`full-frame` または `roi-list`） |
| `model-instance-id` | `model-instance-id`（要素間で実行器を共有） |
| `pre-process-backend` | なし。前処理は常に VA video processor と HIP で行い、`require-zero-copy=true` です |
| `nireq`、`ie-config`、`cpu-throughput-streams` | なし。`batch-size` と `batch-timeout` を使ってください |
| `object-class`、`labels-file` | ラベルは model-info で指定した `labels` ファイルから読み込みます |

### `gvametaconvert` / `gvametapublish`

要素は DL Streamer のソースそのものなので、プロパティは変わりません。同梱版のビルドには、ファイル、MQTT、Kafka の送信機能が含まれます。

## 例

DL Streamer:

```bash
gst-launch-1.0 filesrc location=in.mp4 ! decodebin3 ! \
  gvadetect model=yolov8n.xml model-proc=yolov8n.json device=GPU batch-size=1 ! \
  gvatrack tracking-type=short-term-imageless ! \
  gvawatermark ! videoconvert ! x264enc ! mp4mux ! filesink location=out.mp4
```

このフレームワーク:

```bash
gst-launch-1.0 -e filesrc location=in.h264 ! h264parse ! vah264dec ! \
  'video/x-raw(memory:VAMemory),format=NV12' ! \
  amddetect model=yolov8n.mxr model-info=yolov8n.json batch-size=1 ! \
  gvatrack tracking-type=short-term-imageless ! \
  amdwatermark ! amdencode bitrate=6000 ! h264parse ! mp4mux ! filesink location=out.mp4
```

想定される違い:

- VA デコーダーでデコードし、caps を `video/x-raw(memory:VAMemory)` に制限してください。AMD の要素はシステムメモリ上のフレームを受け付けません。
- `videoconvert` は使いません。フレームがシステムメモリに届くことはありません。`amdwatermark` が出力する GPU サーフェスを `amdencode` が直接エンコードします。
- 生の `.h264` ファイルにはタイムスタンプがありません。`h264parse` の後に
  `video/x-h264,framerate=30/1` を挿入してください（MP4/RTSP のソースでは不要です）。
- AMD の要素のプロパティは NULL/READY 状態でのみ書き込めます。

## モデル

1. モデルを ONNX にエクスポートします（`scripts/export_*.py` の補助スクリプトに、YOLOv8、ResNet18、SegFormer の手順があります）。
2. 入力テンソルと出力のレイアウトを記述した model-info JSON を書きます（[model-info](model-info.ja.md) を参照）。これが `model-proc` の代わりになります。
3. 対象の GPU とバッチサイズに合わせてコンパイルします:
   `amdva-compile-model model.onnx model.mxr --batch=4 --model-info=model.json`。コンパイラーは SHA256 と GPU アーキテクチャを記録した `model.mxr.manifest.json` を書き出します。ランタイムは、マニフェストがない場合や一致しない場合にモデルを拒否します。

OpenVINO IR ファイル、`model-proc` の変換ツール、独自の後処理ライブラリには対応していません。現在利用できるアダプターは `yolov8`、`yolov8-seg`、`yolov8-pose`、分類、セマンティックセグメンテーション、生のテンソルです。YOLOv5 と SSD には対応していません。

## メタデータへのアクセス

- **Python。** `gvapython` は同梱していません。`process(frame_dict)` を実装した `amdpython` を使ってください。[メタデータと Python](metadata-and-python.ja.md) を参照してください。
- **C/C++ のプローブ。** DL Streamer と同じ方法で `GstAnalyticsRelationMeta` を読み取ります。GPU 上にある結果（マスク、生のテンソル）は `AmdAnalysisMeta`（`amdva/tensor_meta.hpp`）に含まれます。暗黙にシステムメモリへコピーされることはありません。
- **GVA の JSON メタ / テンソル。** `gvametaconvert` は GstAnalytics を読み取るため、`amddetect` の出力にも使えます。

## 複数ストリーム

`gvastreammux output-mode=container ! amddetect batch-size=N ! gvastreamdemux`
とすると、解像度の異なるソースを 1 つの検出器でまとめてバッチ処理できます。demux の各分岐にはそれぞれ `queue` を置いてください。[共有バッチと複数ストリーム](framework.ja.md#shared-batches-and-multiple-streams)と `examples/pipelines/multistream-encode.sh` を参照してください。

## ライセンス

同梱した DL Streamer のソースは、MIT ライセンスと Intel の著作権表示をそのまま保持しています（`third_party/dlstreamer/LICENSE`。ローカルでの変更点は `README.AMD.md` に記載しています）。GstAnalytics のグループ/キーポイント API は、同じく DL Streamer に由来する gst-plugins-bad へのパッチによるものです（`patches/gst-plugins-bad-0020-gst-analytics.patch`、LGPL-2.1-or-later のファイル）。`THIRD_PARTY_LICENSES.md` を参照してください。
