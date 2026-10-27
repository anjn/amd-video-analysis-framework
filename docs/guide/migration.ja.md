# 他のフレームワークからの移行

Intel DL Streamer や NVIDIA DeepStream のパイプラインは、要素を置き換えればこのフレームワークで動かせます。どちらも GStreamer と GstAnalytics のメタデータを使う点は同じで、変わるのは推論の要素、モデルの形式、フレームを置くメモリです。このページは対応の早見表で、プロパティ単位の対応は [DL Streamer からの移行](../dev/migration-from-dlstreamer.ja.md) と [DeepStream からの移行](../dev/migration-from-deepstream.ja.md) にあります。

## 要素の対応

| 処理 | DL Streamer | DeepStream | このフレームワーク |
| --- | --- | --- | --- |
| デコード | `decodebin3` など | `nvv4l2decoder` | `vah264dec`、`vah265dec` |
| 検出 | `gvadetect` | `nvinfer`（プライマリ） | `amddetect` |
| 分類 | `gvaclassify` | `nvinfer`（セカンダリ） | `amdclassify` |
| その他の推論 | `gvainference` | `nvinfer` | `amdinference` |
| トラッキング | `gvatrack` | `nvtracker` | `gvatrack`（同梱、画像を使わない方式のみ） |
| 描画 | `gvawatermark` | `nvdsosd` | `amdwatermark` |
| エンコード | `vah264enc` など | `nvv4l2h264enc` | `amdencode` |
| Python | `gvapython` | `pyds` のプローブ | `amdpython`（メタデータのみ） |
| 結果の送信 | `gvametaconvert` ＋ `gvametapublish` | `nvmsgconv` ＋ `nvmsgbroker` | `amdmetapublish`、または同梱の `gvametapublish` |
| 複数ストリーム | `gvastreammux` | `nvstreammux` | `gvastreammux output-mode=container`（同梱） |

## 移行で変わる点

| 項目 | 移行前 | このフレームワーク |
| --- | --- | --- |
| モデル | OpenVINO IR、TensorRT エンジン | ONNX を `amdva-compile-model` でコンパイルした `.mxr` |
| 前処理と出力の設定 | `model-proc`、`config_infer_*.txt` | `model-info.json`（[リファレンス](../dev/model-info.ja.md)） |
| フレームのメモリ | システムメモリ、NVMM | GPU 上の NV12（`video/x-raw(memory:VAMemory),format=NV12`） |
| 形式の変換 | `videoconvert`、`nvvideoconvert` | 不要。推論から描画、エンコードまで GPU 上でつながります |
| バッチサイズ | 実行時に指定 | コンパイル時に固定し、要素の `batch-size` を同じ値にします |
| プロパティの変更 | 実行中も可能なものがある | `amd*` 要素は NULL/READY 状態でのみ変更できます |

対応するモデルは YOLOv8 系の検出・セグメンテーション・姿勢推定、分類、セマンティックセグメンテーション、生のテンソル出力です。YOLOv5、SSD、独自の後処理ライブラリには対応していません。

## パイプラインの書き換え例

DeepStream の検出、トラッキング、描画、エンコードのパイプラインは、次のように書き換えます。

```bash
# DeepStream
gst-launch-1.0 uridecodebin uri=file:///in.mp4 ! m.sink_0 \
  nvstreammux name=m batch-size=1 width=1920 height=1080 ! \
  nvinfer config-file-path=config_infer_primary.txt ! \
  nvtracker ll-lib-file=libnvds_nvmultiobjecttracker.so ll-config-file=tracker.yml ! \
  nvvideoconvert ! nvdsosd ! nvvideoconvert ! nvv4l2h264enc ! h264parse ! mp4mux ! filesink location=out.mp4

# このフレームワーク
gst-launch-1.0 -e filesrc location=in.mp4 ! qtdemux ! h264parse ! vah264dec ! \
  'video/x-raw(memory:VAMemory),format=NV12' ! \
  amddetect model=model.mxr model-info=model-info.json ! \
  gvatrack tracking-type=short-term-imageless ! \
  amdwatermark ! amdencode ! h264parse ! mp4mux ! filesink location=out.mp4
```

ストリームを束ねる要素は、入力が 1 本なら不要です。複数の入力をまとめる方法は [パイプラインの作り方](pipelines.ja.md) を参照してください。
