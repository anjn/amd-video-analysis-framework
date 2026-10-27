<!-- source: docs/migration-from-deepstream.md 0466ebe15828f37213ff5d014b6e649738c5b7a9 -->
# NVIDIA DeepStream からの移行

DeepStream もこのフレームワークも、メタデータモデルを備えた GStreamer パイプラインです。主な違いは次のとおりです。推論は TensorRT ではなく MIGraphX を通して AMD GPU 上で実行します。フレームは NVMM ではなく VA サーフェス（`memory:VAMemory`）に置きます。モデルごとの設定は
`config_infer_*.txt` ではなく小さな JSON ファイル（`model-info`）に記述します。

## 要素の対応表

| DeepStream | このフレームワーク | 備考 |
| --- | --- | --- |
| `nvv4l2decoder` | `vah264dec` / `vah265dec` / `vaav1dec` | 出力は `video/x-raw(memory:VAMemory),format=NV12` |
| `nvstreammux` | `gvastreammux output-mode=container`、または `model-instance-id` を共有する独立した分岐 | コンテナーモードでは各ソースの解像度を保持します。共通のバッチサイズに合わせる `width`/`height` の拡大縮小はありません。 |
| `nvinfer`（プライマリ） | `amddetect` / `amdinference` | |
| `nvinfer`（セカンダリ、`process-mode=2`） | `amdclassify` | `inference-region=roi-list` で、上流の検出器が出力したボックス（最大 300 個）を分類します |
| `nvtracker` | `gvatrack tracking-type=short-term-imageless` | NvDCF、IOU、DeepSORT の低レベルライブラリは利用できません。画像を使わない IOU/Kalman トラッカーのみです。 |
| `nvdsosd` | `amdwatermark` | ボックス、ラベル、トラック ID、マスク、COCO-17 の骨格を GPU で描画します |
| `nvvideoconvert` | 不要。形式や拡大縮小を明示する場合は `vapostproc` | `amd*` 要素は NV12 の VAMemory を直接受け取ります |
| `nvmsgconv` + `nvmsgbroker` | `amdmetapublish`（MQTT、Kafka、ファイル） | JSON スキーマが異なります。[メタデータ](metadata-and-python.ja.md)を参照してください。AMQP、Azure IoT、Redis は提供していません。 |
| `nvdsanalytics`（ROI、ライン通過、方向） | 提供していません | `amdpython` またはプローブで、メタデータを使って実装してください。 |
| `nvdspreprocess` | 推論要素に内蔵 | VA video processor での切り出し/拡大縮小と HIP での正規化 |
| `nvv4l2h264enc` / `nvv4l2h265enc` | `amdencode` | [エンコードと配信](encode-and-serve.ja.md)を参照してください |
| `nvstreamdemux` | `gvastreamdemux` | |
| `nvdsmetamux`、`nvof`、`nvdspostprocess` | 提供していません | |

## `nvinfer` の設定

| `config_infer_*.txt` / プロパティ | このフレームワーク |
| --- | --- |
| `onnx-file`、`model-engine-file` | `model` = `amdva-compile-model model.onnx model.mxr` で生成した `.mxr`。TensorRT エンジンはインポートできません。 |
| `net-scale-factor`、`offsets` | model-info の `input.scale`、`input.mean`（`offset * scale`） |
| `model-color-format` | `input.channel_order` |
| `maintain-aspect-ratio`、`symmetric-padding` | `input.letterbox`（letterbox は対称パディングを行います） |
| `infer-dims` | `input.shape` |
| `num-detected-classes`、`labelfile-path` | `output.num_classes`、`labels` |
| `network-type`（0 検出器、1 分類器、2 セグメンテーション） | `task_type` と要素の選択 |
| `batch-size` | 要素の `batch-size` = コンパイル時のバッチ（`--batch`） |
| `interval`（**スキップする**フレーム数） | `inference-interval`（**N フレームごと**に推論）: `interval=2` は `inference-interval=3` になります |
| `gie-unique-id`、`operate-on-gie-id` | パイプライン上の順序。`model-instance-id` は実行器を共有するだけです |
| `operate-on-class-ids`、`filter-out-class-ids` | 利用できません。コールバックで絞り込んでください |
| `pre-cluster-threshold`、`nms-iou-threshold`、`cluster-mode` | `confidence-threshold`、`nms-threshold`（NMS のみ） |
| `parse-bbox-func-name`、`custom-lib-path` | 利用できません。アダプターは `yolov8`、`yolov8-seg`、`yolov8-pose`、分類、セマンティックセグメンテーション、生のテンソルです |
| `network-mode`（FP32/FP16/INT8） | `amdva-compile-model --precision=fp16|fp32`。INT8 には対応していません |
| `output-tensor-meta` | `task_type=raw` で、出力テンソルを `AmdAnalysisMeta` に入れたまま GPU 上に保持します |
| `tensor-meta-pool-size`、`workspace-size` | なし |

DeepStream の例:

```bash
gst-launch-1.0 uridecodebin uri=file:///in.mp4 ! m.sink_0 \
  nvstreammux name=m batch-size=1 width=1920 height=1080 ! \
  nvinfer config-file-path=config_infer_primary.txt ! \
  nvtracker ll-lib-file=libnvds_nvmultiobjecttracker.so ll-config-file=tracker.yml ! \
  nvvideoconvert ! nvdsosd ! nvvideoconvert ! nvv4l2h264enc ! h264parse ! mp4mux ! filesink location=out.mp4
```

このフレームワーク:

```bash
gst-launch-1.0 -e filesrc location=in.h264 ! h264parse ! vah264dec ! \
  'video/x-raw(memory:VAMemory),format=NV12' ! \
  amddetect model=model.mxr model-info=model-info.json batch-size=1 ! \
  gvatrack tracking-type=short-term-imageless ! \
  amdwatermark ! amdencode bitrate=6000 ! h264parse ! mp4mux ! filesink location=out.mp4
```

途中でデータが GPU の外に出ることがないため、`nvvideoconvert` はありません。

## バッチ処理と複数ストリーム

DeepStream では、すべてのソースを `nvstreammux` に入れてバッチ化します。このフレームワークでは、各分岐に、同じ `model-instance-id`、同じモデル、同じ `batch-size` を設定した推論要素を置きます。共有の実行器は、`batch-timeout`（既定値 20 ms）ごとに全分岐から 1 つのバッチを組み立てます。1 つのバッチにはソースごとに 1 リクエストを入れ、不完全なバッチは GPU 上でパディングします。解像度の異なるソースには `gvastreammux output-mode=container ! amddetect ! gvastreamdemux` を使い、demux の各分岐にそれぞれ `queue` を置いてください。`examples/pipelines/multistream-encode.sh` を参照してください。

`batched-push-timeout` と `live-source` に直接対応するプロパティはありません。ライブの RTSP
ソースでは、mux に `max-fps` を設定しないでください。

## メタデータ

| DeepStream | このフレームワーク |
| --- | --- |
| `NvDsBatchMeta`、`NvDsFrameMeta` | 1 フレームにつき 1 バッファー。フレーム識別情報（ソース、世代、シーケンス、PTS）は `AmdAnalysisMeta` に含まれます |
| `NvDsObjectMeta`（`rect_params`、`confidence`、`class_id`、`object_id`） | `GstAnalyticsTrackingMtd` と関連付けた `GstAnalyticsODMtd`（ボックス、スコア、ラベルの quark） |
| 分類器のメタ | 親オブジェクトと関連付けた `GstAnalyticsClsMtd` |
| マスク / セグメンテーションのメタ | `AmdAnalysisMeta` 内の GPU テンソル |
| `NvDsUserMeta` のテンソル出力 | `task_type=raw`。テンソルは GPU 上に保持します |
| `pyds` のプローブ | `amdpython`（`frame_dict`、読み取り専用のメタデータ）または C++ のパッドプローブ |
| `pyds.get_nvds_buf_surface`（画素へのアクセス） | 設計上利用できません。C++ の GPU API を使ってください |

## メモリ

| DeepStream | このフレームワーク |
| --- | --- |
| `video/x-raw(memory:NVMM)` | `video/x-raw(memory:VAMemory)`（デコーダー、推論、エンコーダー） |
| `nvbuf-memory-type` | なし |
| CUDA ユニファイドメモリ | モデル実行器が所有する HIP デバイスバッファー。VA サーフェスは DMA-BUF 経由で HIP にインポートします |

AMD の推論要素は NV12 の VAMemory（またはそれを格納した gvastreammux のコンテナー）だけを受け付け、既定値は `require-zero-copy=true` です。そのため、システムメモリ上のフレームを黙ってコピーする代わりに、caps ネゴシエーションが失敗します。

## 提供していない機能

INT8 キャリブレーション、TensorRT エンジンのインポート、`nvinferserver`/Triton、NvDCF と
DeepSORT のトラッカー、ReID によるトラッカー機能、`nvdsanalytics`、独自のバウンディングボックスパーサー、音声、Smart Record。人物の ReID と意味検索は
[NVR サンプル](../examples/nvr/README.ja.md)にあり、フレームワークの要素としては提供していません。
