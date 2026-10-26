# Metadata, JSON output and Python callbacks

## Where results live

| Data | Storage | Access |
| --- | --- | --- |
| Boxes, classes, tracking IDs, classifications, keypoints | Standard `GstAnalyticsRelationMeta` (object-detection, classification, tracking, group and keypoint mtds) | `gst_buffer_get_analytics_relation_meta`, as with DL Streamer |
| Instance masks, semantic masks, raw output tensors | `AmdAnalysisMeta` (`amdva/tensor_meta.hpp`), GPU memory | C++ SDK; never copied to the CPU implicitly |
| Frame identity (source, generation, sequence, PTS), inverse image geometry | `AmdAnalysisMeta` | C++ SDK |
| Custom per-frame JSON | `AmdAnalysisMeta::custom_json` (64 KiB limit) | `amdpython`, serialized under `custom` |

`GstAnalyticsGroupMtd` and `GstAnalyticsKeypointMtd` need the GStreamer
gst-plugins-bad patch shipped in `patches/` (`scripts/bootstrap.py` applies it).

## JSON schema 1

`amdmetapublish`, `amdpython` and the SDK produce the same record
(`analysis_json`, `src/gst/publishing.cpp`). Image buffers, masks and GPU
pointers are never serialized.

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

- Boxes are integer pixels in the decoded frame.
- `tracking_id` appears only after a tracker (`gvatrack`) ran.
- `classifications` on an object come from `amdclassify` in ROI mode; the
  top-level array holds full-frame classifications.
- `source`, `generation`, `sequence` are set by applications (the controller sets
  them per channel); in plain pipelines `source` is empty.
- `custom` appears only when `amdpython` attached a dict.

Comparison with other converters: `gvametaconvert` emits DL Streamer's own JSON
(`resolution`, `objects[].detection.bounding_box` normalized, `tensors`), and
DeepStream's `nvmsgconv` emits the Schema 1.0 `object`/`videoPath` layout. Neither
is produced here. If downstream consumers expect one of those, either keep
`gvametaconvert` in the pipeline (it reads GstAnalytics) or reshape the record in
an `amdpython` callback.

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

| Return value | Effect |
| --- | --- |
| `None` or `True` | pass the frame unchanged |
| `False` | drop the frame |
| JSON-serializable `dict` | attached as `custom` (at most 64 KiB) |
| exception | bus error |

The callback runs on the streaming thread under the Python GIL. It sees the
dictionary above and nothing else: no pixels, no device pointers, no GStreamer
objects. That is deliberate, so Python can never trigger a copy of the frame
out of GPU memory. Slow callbacks reduce throughput; keep them short.

If you used `gvapython` with `VideoFrame`/`RegionOfInterest` objects, move the
logic to the dictionary or to a C++ pad probe.

## `amdmetapublish`

| Property | Default | Notes |
| --- | --- | --- |
| `method` | `none` | `none`, `file`, `mqtt`, `kafka` |
| `file-path` | | JSON lines, appended |
| `address` | | MQTT `tcp://host:1883`, Kafka `host:9092` |
| `topic` | `amdva/analytics` | |
| `queue-limit` | 256 | records; full queues discard the oldest waiting record |
| `delivered`, `dropped`, `errors` | read-only | live counters |

Publication is asynchronous and bounded: broker outages never stall video. MQTT
uses QoS 1 and Kafka uses delivery callbacks; this is best-effort delivery, not
exactly-once storage. To keep DL Streamer's `gvametapublish` instead, use the
vendored element with `gvametaconvert` in front of it.

## C++ SDK

```cpp
#include <amdva/publishing.hpp>   // analysis_json(), PublicationOptions, ResultPublisher
#include <amdva/tensor_meta.hpp>  // AmdAnalysisMeta, AnalysisMerge
```

Attach your own data by adding a `GstMeta`/`GstAnalytics` entry in a pad probe;
use `get_analysis_meta(buffer)` to reach the AMD payload and
`AnalysisMerge` semantics (`amd-analytics-merge`) when a custom element merges
branches. Copies of `AmdAnalysisMeta` clone the attachment list and share GPU
storage, so buffers may outlive the model that produced them.
