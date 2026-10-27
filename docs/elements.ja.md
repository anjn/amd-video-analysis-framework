<!-- source: docs/elements.md 0fce7dbba5878c5f0b2e51d90d36deeb952741ff -->
# 要素リファレンス

同梱の DL Streamer 要素（後述）を除き、すべての要素は `amdanalytics` プラグイン（`libgstamdanalytics.so`）に含まれます。ランクは NONE のため、自動で接続されることはありません。*READY* と記したプロパティは NULL/READY 状態でのみ変更できます。正確な一覧は、お使いのビルドで
`gst-inspect-1.0 <element>` を実行して確認してください。

`GST_PLUGIN_PATH` にプラグインのディレクトリ（`build/plugins` または
`<prefix>/lib/gstreamer-1.0`）を設定してください。

## 推論: `amddetect`、`amdclassify`、`amdinference`

実装は 1 つで、既定値だけが異なる 3 つの要素です。インプレースで動作し、メタデータを追加するだけで画素は変更しません。

| | `amddetect` | `amdclassify` | `amdinference` |
| --- | --- | --- | --- |
| `inference-region` の既定値 | `full-frame` | `roi-list` | `full-frame` |
| タスク | 検出、インスタンスセグメンテーション、姿勢推定 | 分類 | 任意の `task_type`（セマンティックセグメンテーション、生のテンソルなど） |

パッド（sink と src）: `video/x-raw(memory:VAMemory),format=NV12,width=[2,32766],height=[2,32766]`
または `multistream/x-analytics-batch(meta:GstAnalyticsBatchMeta)`（`gvastreammux` が生成するコンテナー）。フレームは BT.709 limited range で、幅と高さが偶数である必要があります。

| プロパティ | 型 / 範囲 | 既定値 | 意味 |
| --- | --- | --- | --- |
| `model` | string | | `.manifest.json` を伴うコンパイル済み `.mxr` モデル（READY） |
| `model-info` | string | | [model-info](model-info.ja.md) JSON（READY） |
| `model-instance-id` | string | | ID、モデル、設定が同じ要素は 1 つの実行器を共有し、まとめてバッチ処理します |
| `inference-region` | `full-frame` \| `roi-list` | 上記参照 | `roi-list` では上流の `GstAnalytics` ボックスを最大 300 個まで VPE で切り出します |
| `device` | 0-16 | 0 | HIP デバイス番号 |
| `batch-size` | 1-16 | 1 | コンパイル時のバッチと一致させる必要があります |
| `batch-timeout` | 0-10000 ms | 20 | 不完全なバッチを待つ時間 |
| `inference-interval` | 1-100000 | 1 | N フレームごとに推論します |
| `confidence-threshold` | 0-1 | 0.25 | 物体スコアの最小値 |
| `nms-threshold` | 0-1 | 0.45 | NMS の IoU |
| `require-zero-copy` | bool | true | 画像やテンソルを CPU でコピーする経路をすべて拒否します |

付加するメタデータは、`GstAnalytics` の物体検出、分類、キーポイントグループの mtd と、GPU のマスク/テンソル、逆変換のジオメトリ、フレーム識別情報を保持する `AmdAnalysisMeta` です。キューのあふれはエラーとして扱い、際限なくバッファーリングすることはしません。

## `amdwatermark`

ボックス、ラベル、トラッキング ID、インスタンス/セマンティックのマスク、COCO-17 の骨格を
GPU で描画します。

| | |
| --- | --- |
| Sink | `video/x-raw(memory:VAMemory),format=NV12,width=[2,4096],height=[2,4096]` |
| Src | `video/x-raw(memory:DMABuf),format=BGRA,width=[2,4096],height=[2,4096]`（リニア、エクスポートした VA サーフェス） |
| `device` | HIP デバイス番号（既定値 0、READY） |
| `require-zero-copy` | 既定値 true（READY） |

BGRA サーフェスは EGL（`amdva::EglDmaBufImporter`）で表示するか、`amdencode` でエンコードできます。通常の `vapostproc` ではこのサーフェスをネゴシエーションできません。

## `amdencode`

VA のハードウェアエンコーダーを内包する bin です。[エンコードと配信](encode-and-serve.ja.md)を参照してください。

| | |
| --- | --- |
| Sink | `video/x-raw(memory:VAMemory),format={NV12,BGRA,BGRx,RGBA}` または `video/x-raw(memory:DMABuf),format={NV12,BGRA,BGRx,RGBA}` |
| Src | `video/x-h264` または `video/x-h265` の byte-stream（AU 単位）、`video/x-av1` の OBU/TU |
| `codec` | `h264`（既定値）、`h265`、`av1` |
| `bitrate` | 1-2048000 kbit/s（既定値 4000） |
| `key-int-max` | 1-10000（既定値 30） |
| `b-frames` | 0-4（既定値 0。AV1 では使用不可） |
| `signal-bt709` | 既定値 true。H.264 の SPS VUI を書き換えます |

すべてのプロパティは READY です。内部構成は、DMA-BUF のインポートと変換の段、`vapostproc`、caps フィルター、`vah264enc`/`vah265enc`/`vaav1enc`、パーサーです。

## `amdpython`

メタデータのみを扱うコールバックです。[メタデータと Python](metadata-and-python.ja.md) を参照してください。

| プロパティ | 既定値 | |
| --- | --- | --- |
| `module` | | Python のソースファイル（READY） |
| `function` | `process` | `callback(frame_dict)`（READY） |

caps: 任意（パススルー）。

## `amdmetapublish`

上限付きの非同期送信要素です。caps は任意で、パススルーです。

| プロパティ | 既定値 | |
| --- | --- | --- |
| `method` | `none` | `none`、`file`、`mqtt`、`kafka` |
| `file-path` | | JSON Lines の出力先 |
| `address` | | ブローカーのアドレス |
| `topic` | `amdva/analytics` | |
| `queue-limit` | 256 | 1-65536 レコード |
| `delivered`、`dropped`、`errors` | 読み取り専用 | uint64 のカウンター |

## 互換用の組: `amdpreproc`、`amdmigraphx`

当初の 2 要素構成の YOLOv8 インターフェースで、互換性のために残しています（`amdpreproc`: `device`、`tensor-width`、`tensor-height`、`model-info`、`require-zero-copy`。`amdmigraphx`: `model`、`model-info`、`device`、`confidence-threshold`、`nms-threshold`、`require-zero-copy`）。新しいパイプラインでは
`amddetect` を使ってください。タスク要素の前に `amdpreproc` を置かないでください。

## 同梱の DL Streamer 要素

`third_party/dlstreamer` からビルドします（`AMDVA_BUILD_GVA_COMPAT`）。プロパティは DL Streamer v2026.2.0 のものです。

| 要素 | このフレームワークでの用途 |
| --- | --- |
| `gvatrack` | `tracking-type=short-term-imageless` または `zero-term-imageless`。CPU のみで動作し、画像をマップしません |
| `gvametaconvert` | GstAnalytics を DL Streamer の JSON に変換します（`format=json`、`json-indent`、`source`、`tags`） |
| `gvametapublish` | `method=file|mqtt|kafka`、`file-path`、`address`、`topic` |
| `gvastreammux` | `output-mode=container` で解像度の異なる複数ストリームをバッチ化します。`max-queue-size`、`max-wait-time`、`sync-mode` |
| `gvastreamdemux` | コンテナーをソースごとの分岐に戻します |
| `gvametaaggregate` | 並列の分岐をまとめ、AMD の GPU ペイロードも統合します |

## 典型的なパイプライン

```text
filesrc ! h264parse ! vah264dec ! video/x-raw(memory:VAMemory),format=NV12 !
  amddetect model=m.mxr model-info=m.json !
  gvatrack tracking-type=short-term-imageless !
  amdpython module=cb.py ! amdmetapublish method=mqtt address=tcp://broker:1883 !
  amdwatermark ! amdencode ! h264parse ! mp4mux ! filesink location=out.mp4
```
