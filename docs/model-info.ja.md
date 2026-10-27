<!-- source: docs/model-info.md c34ed30f123be24e5bb7a804e4635d575c2cb667 -->
# model-info リファレンス

`model-info` は、コンパイル済みの `.mxr` モデルと同じ場所に置く小さな JSON ファイルです。DL Streamer の `model-proc` や、DeepStream の `config_infer_*.txt` にある前処理と後処理の項目の代わりになります。`ModelInfo::load`（`include/amdva/model.hpp`、`src/model.cpp`）で解析し、すべての `amd*` 推論要素と SDK で共通に使います。

```json
{
  "task_type": "detection",
  "adapter": "yolov8",
  "input": {
    "name": "images",
    "shape": [1, 3, 640, 640],
    "format": "NCHW",
    "channel_order": "RGB",
    "letterbox": true,
    "scale": 0.00392156862,
    "mean": [0, 0, 0],
    "std": [1, 1, 1],
    "padding": 114
  },
  "output": { "format": "cxcywh", "num_classes": 80, "top_k": 5, "keypoints": 17 },
  "labels": "coco80.txt"
}
```

## 項目

| 項目 | 既定値 | 意味 |
| --- | --- | --- |
| `task_type` | `detection` | `detection`、`instance-segmentation`、`pose`、`classification`、`semantic-segmentation`、`raw` のいずれか |
| `adapter`（別名 `family`） | 検出、セグメンテーション、姿勢推定では `yolov8`。それ以外はタスク名 | 出力のデコーダー。検出用のアダプターは `yolov8`、`yolov8-seg`、`yolov8-pose` です。YOLOv5、SSD、独自のパーサーには対応していません。 |
| `input.name` | `images` | モデルの入力テンソル名 |
| `input.shape` | 必須 | `[N,3,H,W]`。N は 1-16、チャンネル数は 3、H と W は 2-4096。固定形状のみです。 |
| `input.format` | `NCHW` | `NCHW` のみ受け付けます |
| `input.channel_order` | `RGB` | `RGB` または `BGR` |
| `input.letterbox` | `true`。分類とセマンティックセグメンテーションでは `false` | アスペクト比を保ってパディングするか、直接リサイズするか |
| `input.scale`、`mean`、`std` | `1/255`、`[0,0,0]`、`[1,1,1]` | 上記のチャンネル順で、チャンネルごとに `(pixel * scale - mean) / std` として適用します。`std` は正の値である必要があります。 |
| `input.padding` | `114` | レターボックスのパディング値 |
| `input.normalize` | なし | 従来の項目で、`[0, 1]` のみ受け付けます。代わりに `scale`/`mean`/`std` を使ってください。 |
| `output.num_classes` | 80 | クラス数（1-65536）。ラベルファイルの行数と一致する必要があります。 |
| `output.top_k` | 5 | 保持する分類結果の数（1-100） |
| `output.keypoints` | 17 | 姿勢推定での 1 人あたりのキーポイント数（1-64） |
| `output.format` | 参考情報 | モデルが出力するデコード済みボックスの形式（YOLOv8 では `cxcywh`） |
| `labels` | インデックスの文字列 | 1 行に 1 ラベルを記述したテキストファイル。JSON ファイルからの相対パス |

YOLO アダプターは、デコード済みの中心/サイズ形式のボックスとクラス確率を想定しており、独立した
objectness チャンネルは扱いません（`[B, 4+C, N]`）。セグメンテーションでは `[B,M,H,W]` のプロトタイプが加わり、姿勢推定では K x 3 のキーポイントチャンネルが加わります。分類ではロジット
`[B,C]`、セマンティックセグメンテーションでは `[B,C,H,W]` を想定します。

## コンパイル

```bash
amdva-compile-model model.onnx model.mxr --batch=4 --precision=fp16 \
  --model-info=model-info.json [--input-shape=NAME:D1,D2,...]
```

コンパイラーは、SHA256、GPU アーキテクチャ、バッチサイズ、`offload_copy=false` を記録した
`model.mxr.manifest.json`（スキーマ 2）を書き出します。読み込み時にランタイムは、マニフェストがないモデル、ハッシュが異なるモデル、別の GPU アーキテクチャ向けにビルドしたモデルを拒否します。GPU やバッチサイズを変えた場合は再ビルドしてください。`--precision=fp16`（既定）で変わるのは内部演算だけで、外部のテンソルは元の型のままです。

要素は float32 NCHW の画像入力を 1 つだけ要求します。複数入力のモデルや他のテンソル型（float16、int32、int64、uint8）は、C++ の
`ModelExecutor` API から使えます。

## 他の形式との対応

| 他の形式 | 対応する項目 |
| --- | --- |
| DL Streamer `model-proc` `input_preproc.resize`, `color_space` | `input.letterbox`, `input.channel_order` |
| DL Streamer `model-proc` `output_postproc.converter` | `adapter` / `task_type` |
| DL Streamer `labels` | `labels` |
| DeepStream `net-scale-factor` | `input.scale` |
| DeepStream `offsets` | `input.mean`（スケール後の単位: `offset * scale`） |
| DeepStream `model-color-format` (0 RGB, 1 BGR) | `input.channel_order` |
| DeepStream `maintain-aspect-ratio` | `input.letterbox` |
| DeepStream `num-detected-classes` | `output.num_classes` |
| DeepStream `labelfile-path` | `labels` |
| DeepStream `batch-size` | 要素の `batch-size` と `amdva-compile-model --batch` |
