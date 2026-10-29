<!-- source: docs/dev/encode-and-serve.md 2c6e901cac08be989a6c0cd940c45f221ae8b441 -->
# エンコード、録画、配信

このフレームワークは、デコードしたフレームを GPU メモリ上に保持します。このページでは、CPU で画素に触れずに、描画済みのストリームを圧縮映像として出力する方法を説明します。

## ゼロコピーの経路

```text
vah264dec -> VAMemory NV12 -> amddetect -> amdwatermark -> BGRA DMA-BUF
          -> amdencode ( import -> VA video processor -> NV12 VAMemory -> vah26xenc / vaav1enc )
          -> compressed access units (CPU: parsing, muxing, RTP only)
```

- `amdwatermark` は NV12 の VAMemory フレームに描画し、DMA-BUF としてエクスポートしたリニアな BGRA サーフェスを出力します。GStreamer の `vapostproc` はこの旧来の
  `memory:DMABuf, format=BGRA` 形式をネゴシエーションできないため、`amdencode` が自身で
  DMA-BUF を VA サーフェスとしてインポートし（コピーなし）、VA video processor 上で
  NV12 limited-range BT.709 に変換します。
- VAMemory の入力（NV12、BGRA、BGRx、RGBA）は `vapostproc` を通ります。すでに NV12 の場合はそのまま通過します。
- CPU に届くのは圧縮済みのビットストリームだけです。ご自身のパイプラインでこれを確認するには、コピー監査ツール（`amdva/copy_audit.hpp`、`libamdva_copy_audit.so`）を使います。

### CPU で生成したフレーム

システムメモリ上で生成したフレーム（たとえば `appsrc` で送り込む OpenCV の `Mat`）は、エンコーダーで使う前に VA サーフェスへアップロードする必要があります。このアップロードは本質的に CPU から GPU へのコピーです。ゼロコピーではなく、フレームワークはこれを隠しません。アップロードは 1 回だけにし（`vapostproc` はシステムメモリの入力を受け付け、VAMemory を出力します）、以降の段はすべて GPU 上に置いてください。GPU でフレームを生成する場合は、代わりに次に示す VA/EGL の経路を使ってください。

### GPU で描画したフレーム

独自のグラフィックス（ダッシュボードや、`amdwatermark` では描けないオーバーレイ）を描画するには、BGRA の VA サーフェスに描画してエンコードします。

1. `VaSession::create_bgra` で確保し、`export_surface(surface, true)` でエクスポートします。
2. `amdva::EglDmaBufImporter`（`amdva/egl_import.hpp`）で EGL にインポートし、テクスチャを
   FBO にアタッチして描画し、GL フェンスで完了を待ちます。
3. VA video processor 上で NV12 に変換して `gst_va_pool` のバッファーに書き込み、エンコーダーにつながる `appsrc` に送ります。完全なサンプルは `examples/resource-dashboard/renderer.cpp` です。

Mesa 25.2.x に関する注意: RGB サーフェスを同一の領域どうしで NV12 に変換すると、16 フレーム後に何の通知もなくエンコーダー側の変換に切り替わり、その後失敗します。ダッシュボードは 1920x1082 のソースを確保し、2-1081 行目に描画して、その矩形を 1920x1080 に変換することで、領域が異なるようにしています。`amdencode` はオーバーレイからインポートしたサーフェスを変換しますが、描画済みストリームの 600 フレームでこの失敗が起きないことを検証しています。ただし、この経路での正確な発生条件は解明されていません。Mesa 25.2.x で独自の変換を書く場合は、変換元と出力の領域を異なるものにしてください。

## `amdencode`

```bash
... ! amdwatermark ! amdencode codec=h264 bitrate=6000 key-int-max=30 ! h264parse ! mp4mux ! filesink location=out.mp4
```

| プロパティ | 既定値 | 備考 |
| --- | --- | --- |
| `codec` | `h264` | `h264`、`h265`、`av1`（それぞれ `vah264enc`、`vah265enc`、`vaav1enc` が必要） |
| `bitrate` | 4000 | kbit/s。目標値で、`vbr` と `qvbr` では上限になります |
| `rate-control` | `cbr` | `cbr`、`vbr`、`qvbr`（画質を指定する VBR）。`vbr`/`qvbr` は動きの少ない場面で使うビット数を減らします |
| `key-int-max` | 30 | キーフレームの間隔（フレーム数） |
| `b-frames` | 0 | H.264/H.265 のみ。0 にすると遅延を小さく保てます |
| `signal-bt709` | true | BT.709 limited range として符号化し、記録します（すべてのコーデックで caps に、H.264 では SPS の VUI にも） |

プロパティは NULL/READY 状態で書き込めます。出力は access unit 単位の byte-stream です（AV1 では OBU ストリーム）。ソフトウェアへのフォールバックはありません。ハードウェアエンコーダーがない場合、この要素は missing-plugin エラーで状態遷移に失敗します。

`signal-bt709` があるのは、フレームの取り込みで色空間の情報が落ちるため、そのままでは VA エンコーダーがストリームを BT.601 と記録してしまうからです。有効にすると、エンコーダーへの入力の caps が BT.709 になり、それが H.265/AV1 のヘッダーとコンテナーの色の情報に記録されます。H.264 では、さらに `amdva::signal_bt709`（`amdva/vui.hpp`）が SPS の VUI を書き換えます。スライスデータとタイミングには手を加えません。NVR のプレーヤーは BT.709（または色空間の指定がない）録画だけを受け付けます。取り込んだフレームには色空間の情報がないため、映像処理は 720 ライン未満を BT.601 とみなします。元の映像が BT.709 なら、720p 以上で符号化してください。

radeonsi の AV1 エンコーダーは、幅を 64 の倍数に広げ、高さが 16 の倍数でないと大きさを誤ります（1080 は 1082 としてデコードされます）。AV1 には、`Transcoder` と同じく、両方の条件を満たす大きさのフレームを渡してください。

## 録画

`splitmuxsink` で一定の長さのファイルに分割します（`examples/pipelines/annotate-segments.sh` を参照）。

```bash
... ! amdencode key-int-max=30 ! h264parse ! \
  splitmuxsink muxer=matroskamux location=out-%05d.mkv max-size-time=10000000000
```

すべてのファイルがキーフレームから始まるよう、`key-int-max` はセグメントの長さより短くしてください。NVR は、カメラ自身の圧縮ストリームを（再エンコードせずに）録画します。`amdva::streaming::SegmentRecorder`（`amdva/streaming/recorder.hpp`）は、その録画処理を再利用できるクラスにしたものです。キーフレームから始まる H.264/H.265 の access unit を `push()` で渡し、`poll()` を繰り返し呼ぶと、Matroska のセグメントを順次書き出しながら、`Catalog`（名前、メディアの開始時刻、長さ、中断したセグメント）を同期して更新します。保持期間と容量上限は、カタログとコントローラーが管理します（`docs/dev/nvr/vms.md`）。`SegmentRecorder` は AV1（`RecordCodec::AV1`）も書き出せます。途中で遅れが生じる処理を経たサンプルでは、`push(sample, wall_ms)` に最初のサンプルを受信した時刻を渡します。

### 録画の再エンコード

`amdva::streaming::Transcoder`（`amdva/streaming/transcoder.hpp`）は、ストレージを節約するため、1 台のカメラの H.264/H.265 の access unit を GPU で再エンコードします。

```
appsrc ! h26xparse ! vah26xdec [! tee ! appsink frames] ! queue leaky
       ! [vapostproc ! width,height] ! amdencode ! appsink
```

設定は、出力のコーデック（H.264、H.265、AV1）、レート制御、ビットレート、出力の高さ（0 は元の大きさ。拡大はせず、縦横比を保ちます）、キーフレームの間隔です。AV1 の大きさは、幅を 64 の倍数、高さを 16 の倍数にそろえます（1080p は 1920x1088 になります）。受信したサンプルをキーフレームから 1 つのスレッドで `push()` し、同じスレッドで `pull()` から取り出して、最初の出力の caps で開いた `SegmentRecorder` に渡します。エンコーダーの手前の 4 フレームのキューは、動画エンジンが追いつかないと最も古いフレームを捨てるので、デコードが滞る代わりに出力のフレームレートが下がります。出力の数は `encoded()` で数えます。ほかのデコーダーに動画エンジンの時間を残すため、`max_fps()` で実行中にエンコードのフレームレートの上限を設定できます。`push()` が `false` を返すのは、それでも入力のバックログが上限に達したときで、呼び出し側は次のキーフレームからやり直します。`Options::frames` を指定すると、`frames()` がデコード済みの NV12 VAMemory フレーム（最新のもののみ）の appsink になり、表示や解析でデコード結果を共有できます。元の PTS は参照タイムスタンプのメタデータとしてデコードとエンコードを通り抜けるので、出力はカメラの時間軸を保ちます。`latency_ms()` は push から出力までの遅れです。保存した解析結果とずれないよう、最初の出力の PTS を受信した時刻を `SegmentRecorder::push` に渡してください。`transcode-contract` は、生成したストリームを 3 つのコーデックに再エンコードし、再生できることを確かめます。

`amdva::streaming::RtspIngest`（`amdva/streaming/ingest.hpp`）は、これに対応する受信側です。1 台の RTSP カメラから byte-stream 形式で AU 単位の H.264/H.265 の access unit を取り込み、上限付きのキュー（`take()`）に入れます。キューのあふれと 5 秒間の映像タイムアウトは `poll()` で通知し、再接続の方針は呼び出し側に任せます。コントローラーは、受信、録画、VA デコーダーの
3 つの部品をカメラごとに組み合わせます。

## RTSP 配信

`amdva::RtspServer`（`amdva/rtsp_server.hpp`）は、appsrc から供給するストリーム向けに
`gst-rtsp-server` をラップしたものです。`examples/sdk/transcode.cpp` は、デコード、解析、エンコード、配信を行う完全なプログラムです。

```bash
transcode in.h264 model.mxr model-info.json --rtsp 8554 --mount /annotated --codec h264
# rtsp://127.0.0.1:8554/annotated
```

launch 記述では、ソースに `video`、ペイローダーに `pay0` という名前を付けます。

```text
( appsrc name=video is-live=true format=time do-timestamp=true ! h264parse config-interval=-1 !
  rtph264pay name=pay0 pt=96 config-interval=-1 )
```

パーサーに `config-interval=-1` を指定すると、キーフレームの前に毎回 SPS/PPS を繰り返すため、クライアントはストリームの途中から参加できます。配信できるのは H.264 と H.265 です。RTP 経由の
AV1 に対応するクライアントは少ないため、AV1 はファイルに使ってください。

ソースがすでにエンコード済み（ファイルやカメラの再配信）の場合は、デコードせずに配信してください。`amdva-rtsp-files` は H.264/H.265 のファイルに対してまさにこれを行います。ダッシュボード（`docs/dev/resource-dashboard/README.md`）は GPU で描画したストリームを配信し、エンコーダーの出力に BT.709 の書き換えを適用します。

## SDK の部品

| ヘッダー | 用途 |
| --- | --- |
| `amdva/vui.hpp` | H.264 の access unit 向けの `signal_bt709()` |
| `amdva/rtsp_server.hpp` | `RtspServer`: マウント、appsrc ファクトリー、クライアント/セッションの終了処理 |
| `amdva/egl_import.hpp` | `EglDmaBufImporter`: VA サーフェスを EGLImage/GL テクスチャとしてエクスポート |
| `amdva/copy_audit.hpp` | コピー監査ツールの weak フック |

`AMDVA::amdva_media` をリンクしてください。
