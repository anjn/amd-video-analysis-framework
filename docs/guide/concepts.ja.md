# 仕組み

このフレームワークは、デコードした映像を最後まで GPU のメモリに置いたまま処理します。CPU が扱うのは圧縮された映像データと、検出結果などの小さなメタデータだけで、画素のコピーや変換は CPU では行いません。このページでは、データの流れ、要素の役割、モデル、メタデータ、ライブラリの構成を説明します。

## データの流れ

```mermaid
flowchart TB
  IN[圧縮映像<br>RTSP・ファイル] --> DEC
  subgraph GPU[GPU]
    DEC[VA デコード<br>NV12] --> PRE[VPE<br>切り出し・縮小] --> NORM[HIP<br>正規化してテンソルへ] --> INF[MIGraphX<br>推論] --> POST[後処理<br>NMS など]
    DEC --> DRAW[描画] --> ENC[VA エンコード]
  end
  POST --> META[メタデータ<br>枠・クラス・ID]
  META -.描画の指示.-> DRAW
  META --> JSON[JSON・MQTT・Kafka]
  ENC --> OUT[圧縮映像<br>MP4・RTSP]
```

| 段階 | 実行するハードウェア | 内容 |
| --- | --- | --- |
| デコード、エンコード | VCN（VA-API） | H.264/H.265 のデコードと、H.264/H.265/AV1 のエンコード |
| 切り出し、縮小 | VPE（VA の映像処理） | 推論の入力サイズへの変換と、分類用の領域（ROI）の切り出し |
| 正規化 | GPU（HIP） | 画素をモデル入力のテンソル（float32）に変換 |
| 推論、後処理 | GPU（MIGraphX、HIP） | 推論、検出枠の絞り込み（NMS）、マスクやキーポイントの復元 |
| 描画 | GPU（VPE、HIP） | 検出枠、ラベル、ID、マスク、骨格の描画 |

セグメンテーションのマスクや推論の生の出力は、GPU 上のテンソルのまま保持されます。CPU に読み戻すのは、枠の座標やクラスなど上限のある結果だけです。

## 要素の役割

パイプラインは GStreamer の要素をつないで作ります。このフレームワークが提供する要素は次のとおりです。

| 要素 | 役割 |
| --- | --- |
| `amddetect` | 物体検出、インスタンスセグメンテーション、姿勢推定（YOLOv8 系） |
| `amdclassify` | 検出した物体ごと、またはフレーム全体の分類 |
| `amdinference` | セマンティックセグメンテーションなど、任意のモデルの推論 |
| `amdwatermark` | 検出結果の描画 |
| `amdencode` | ハードウェアエンコード |
| `amdpython` | 検出結果を受け取る Python 関数の呼び出し（画素は渡しません） |
| `amdmetapublish` | 検出結果を JSON Lines のファイル、MQTT、Kafka へ送信 |
| `gvatrack` ほか `gva*` | Intel DL Streamer から同梱したトラッキング、複数ストリームの束ね、メタデータ変換 |

推論の要素は、入力として GPU 上の NV12（`video/x-raw(memory:VAMemory),format=NV12`）だけを受け付けます。CPU 上のフレームを黙ってコピーすることはなく、つなぎ方が誤っていれば起動時にエラーになります。

## モデル

ONNX のモデルを `amdva-compile-model` でこの GPU 向けにコンパイルし、`.mxr` ファイルとして使います。前処理と出力の形式は、モデルと並べて置く `model-info.json` に書きます。

- バッチサイズは 1、2、4、8、16 のいずれかで、コンパイル時に固定します。
- コンパイルするとハッシュと GPU を記録したマニフェストも作られ、別の GPU 向けのモデルは読み込み時に拒否されます。
- 対応する出力形式は、YOLOv8 系の検出・セグメンテーション・姿勢推定、分類、セマンティックセグメンテーション、生のテンソルです。

`model-info.json` の項目は [model-info リファレンス](../dev/model-info.ja.md) を参照してください。

## メタデータ

検出結果は GStreamer 標準の GstAnalytics メタデータとしてフレームに付きます。下流の要素や自作のプローブは、DL Streamer と同じ方法で読み取れます。GPU 上のマスクやテンソル、フレームの識別情報は、このフレームワーク独自のメタデータに入ります。

`amdmetapublish` と `amdpython` は、同じ形式の JSON（スキーマ 1）を扱います。1 フレームが 1 レコードで、物体ごとにクラス、信頼度、枠、トラッキング ID、分類、キーポイントを持ちます。形式の詳細は [メタデータと Python](../dev/metadata-and-python.ja.md) を参照してください。

## ライブラリの構成

アプリケーションから使う場合は、CMake パッケージ `AMDVideoAnalysis` のライブラリをリンクします。矢印は、どのライブラリを使っているかを表します。

```mermaid
flowchart BT
  runtime[amdva_runtime<br>VA と HIP の連携、前処理]
  inference[amdva_inference<br>モデル、バッチ実行]
  gst[amdva_gst_support<br>メタデータ、トラッキング、描画、送信]
  streaming[amdva_streaming<br>RTSP 受信、録画、カタログ、再生]
  media[amdva_media<br>エンコードの補助、RTSP 配信、EGL]
  monitor[amdva_monitor<br>CPU・GPU・電力の計測]
  inference --> runtime
  gst --> inference
  streaming --> gst
```

`amdva_media` と `amdva_monitor` は、ほかのライブラリに依存せずに使えます。使い方は [SDK でアプリケーションに組み込む](sdk.ja.md) で説明します。
