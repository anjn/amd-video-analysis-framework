<!-- source: docs/dev/metadata-and-python.md 6ba483e8e116ac24011e6bdd5669c4eb579c274c -->
# メタデータ、JSON 出力、Python コールバック

## 結果の格納場所

| データ | 格納先 | アクセス方法 |
| --- | --- | --- |
| ボックス、クラス、トラッキング ID、分類、キーポイント | 標準の `GstAnalyticsRelationMeta`（物体検出、分類、トラッキング、グループ、キーポイントの mtd） | DL Streamer と同様に `gst_buffer_get_analytics_relation_meta` |
| インスタンスマスク、セマンティックマスク、生の出力テンソル | `AmdAnalysisMeta`（`amdva/tensor_meta.hpp`）、GPU メモリ | C++ SDK。暗黙に CPU へコピーされることはありません |
| フレーム識別情報（ソース、世代、シーケンス、PTS）、画像の逆変換ジオメトリ | `AmdAnalysisMeta` | C++ SDK |
| フレームごとの独自 JSON | `AmdAnalysisMeta::custom_json`（上限 64 KiB） | `amdpython`。`custom` の下にシリアライズされます |

`GstAnalyticsGroupMtd` と `GstAnalyticsKeypointMtd` を使うには、`patches/` に同梱した GStreamer
gst-plugins-bad のパッチが必要です（`scripts/bootstrap.py` が適用します）。

## JSON スキーマ 1

`amdmetapublish`、`amdpython`、SDK は同じレコードを生成します（`analysis_json`、`src/gst/publishing.cpp`）。画像バッファー、マスク、GPU のポインターはシリアライズしません。

```json
{
  "schema": 1,
  "source": "cam0",
  "generation": 3,
  "sequence": 1042,
  "pts_ns": 34733333333,
  "background": false,
  "objects": [
    {
      "label": "person", "confidence": 0.91,
      "x": 412, "y": 160, "width": 88, "height": 240,
      "tracking_id": 17,
      "classifications": [{"label": "adult", "confidence": 0.8}],
      "keypoints": [{"x": 440, "y": 180, "confidence": 0.9}]
    }
  ],
  "classifications": [{"label": "indoor", "confidence": 0.7}],
  "tensors": [
    {"name": "mask", "storage": "gpu", "dtype": "float32", "device": 0,
     "batch_index": 0, "shape": [3, 160, 160]}
  ],
  "custom": {"people": 1}
}
```

- ボックスは、デコードしたフレーム上の整数のピクセル座標です。
- `tracking_id` は、トラッカー（`gvatrack`）を通した後にのみ現れます。
- 物体の `classifications` は ROI モードの `amdclassify` によるものです。最上位の配列にはフレーム全体の分類が入ります。
- `source`、`generation`、`sequence` はアプリケーションが設定します（コントローラーはチャンネルごとに設定します）。単純なパイプラインでは `source` は空です。
- `custom` は、`amdpython` が dict を付加した場合にのみ現れます。

他の変換要素との比較: `gvametaconvert` は DL Streamer 独自の JSON（`resolution`、正規化した
`objects[].detection.bounding_box`、`tensors`）を出力し、DeepStream の `nvmsgconv` は Schema 1.0 の
`object`/`videoPath` 形式を出力します。このフレームワークはどちらも生成しません。下流の利用側がいずれかの形式を前提とする場合は、パイプラインに `gvametaconvert` を残す（GstAnalytics を読み取ります）か、`amdpython` のコールバックでレコードの形を変えてください。

## `amdpython`

```python
# metadata_callback.py
def process(frame):
    people = [o for o in frame['objects'] if o['label'] == 'person']
    return {'people': len(people),
            'tracked_people': [o['tracking_id'] for o in people if 'tracking_id' in o]}
```

```bash
... ! amdpython module=/path/metadata_callback.py function=process ! ...
```

| 戻り値 | 動作 |
| --- | --- |
| `None` または `True` | フレームをそのまま通します |
| `False` | フレームを破棄します |
| JSON にシリアライズできる `dict` | `custom` として付加します（最大 64 KiB） |
| 例外 | バスエラー |

コールバックは、Python の GIL を取得したストリーミングスレッド上で実行します。コールバックが受け取るのは上記の辞書だけで、画素、デバイスポインター、GStreamer のオブジェクトは受け取りません。これは意図的な設計で、Python からフレームを GPU メモリの外へコピーさせないためです。処理の遅いコールバックはスループットを下げるため、短く保ってください。

`VideoFrame`/`RegionOfInterest` オブジェクトを使う `gvapython` を利用していた場合は、その処理を辞書を扱う形に書き換えるか、C++ のパッドプローブに移してください。

## `amdmetapublish`

| プロパティ | 既定値 | 備考 |
| --- | --- | --- |
| `method` | `none` | `none`、`file`、`mqtt`、`kafka` |
| `file-path` | | JSON Lines、追記 |
| `address` | | MQTT は `tcp://host:1883`、Kafka は `host:9092` |
| `topic` | `amdva/analytics` | |
| `queue-limit` | 256 | レコード数。キューが満杯になると、待機中の最も古いレコードを破棄します |
| `delivered`、`dropped`、`errors` | 読み取り専用 | 動作中に更新されるカウンター |

送信は非同期で、上限があります。ブローカーが停止しても映像は止まりません。MQTT は QoS 1 を使い、Kafka は配信コールバックを使います。これはベストエフォートの配信であり、exactly-once の保存ではありません。DL Streamer の `gvametapublish` を使い続ける場合は、同梱の要素を使い、その前に `gvametaconvert` を置いてください。`AMDVA_WITH_MQTT=OFF` または `AMDVA_WITH_KAFKA=OFF` でビルドすると、送信を開始した時点でその method を拒否し、対応する `gvametapublish` のモジュールも含めません。`AMDVA_WITH_PYTHON=OFF` では `amdpython` 要素を登録しません。

## C++ SDK

```cpp
#include <amdva/publishing.hpp>   // analysis_json(), PublicationOptions, ResultPublisher
#include <amdva/tensor_meta.hpp>  // AmdAnalysisMeta, AnalysisMerge
```

独自のデータを付加するには、パッドプローブで `GstMeta`/`GstAnalytics` のエントリを追加します。AMD のペイロードには `get_analysis_meta(buffer)` でアクセスし、独自の要素で分岐をまとめる場合は
`AnalysisMerge` のセマンティクス（`amd-analytics-merge`）を使ってください。`AmdAnalysisMeta` をコピーすると、付加情報のリストは複製され、GPU 上のストレージは共有されます。そのため、バッファーはそれを生成したモデルより長く存続できます。

## 録画と結果の保存

`amdva::streaming::Catalog`（`amdva/streaming/catalog.hpp`）は、解析結果を対象の録画と並べて保存し、録画を削除するときに一緒に削除します。`RecordedAnalysis` は 1 フレーム分の情報を保持します。内容は、チャンネル、セッション、実行、時刻、有効性と、`RecordedObject`（正規化したボックス、クラス ID、人物 ID とトラック ID）のリストです。任意の詳細情報は、設定した場合にのみ保存します。対象は `RecordedObject::label`、`keypoints`（正規化した x、y、信頼度）、`attributes_json`（任意の JSON 値。たとえば分類）、フレーム単位の `RecordedAnalysis::custom_json` です。これらの詳細情報が追加される前に作成したデータベースには、開いたときに NULL 許容の列が追加されます。

```cpp
AnalysisHooks hooks;                                    // optional queue metrics
hooks.on_saved = [](double ms, double wait_ms, size_t frames) { /* ... */ };
catalog.analysis_hooks(std::move(hooks));
catalog.enqueue_analysis(std::move(frame));             // bounded, written in batches
auto frames = catalog.analysis("cam0", from_ms, to_ms); // only for live recordings
```
