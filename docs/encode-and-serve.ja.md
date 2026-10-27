<!-- source: docs/encode-and-serve.md 1d6238bfed9b6e5c31b90e130380e12ad73d10ed -->
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
| `bitrate` | 4000 | kbit/s |
| `key-int-max` | 30 | キーフレームの間隔（フレーム数） |
| `b-frames` | 0 | H.264/H.265 のみ。0 にすると遅延を小さく保てます |
| `signal-bt709` | true | H.264 のみ。SPS の VUI を BT.709 limited range に書き換えます |

プロパティは NULL/READY 状態で書き込めます。出力は access unit 単位の byte-stream です（AV1 では OBU ストリーム）。ソフトウェアへのフォールバックはありません。ハードウェアエンコーダーがない場合、この要素は missing-plugin エラーで状態遷移に失敗します。

`signal-bt709` があるのは、VA エンコーダーが caps から色の VUI フィールドを書き込まないためです。`amdva::signal_bt709`（`amdva/vui.hpp`）は SPS だけを書き換え、スライスデータとタイミングには手を加えません。H.265 と AV1 のストリームは書き換えません。プレーヤーは HD コンテンツを
BT.709 とみなします。

## 録画

`splitmuxsink` で一定の長さのファイルに分割します（`examples/pipelines/annotate-segments.sh` を参照）。

```bash
... ! amdencode key-int-max=30 ! h264parse ! \
  splitmuxsink muxer=matroskamux location=out-%05d.mkv max-size-time=10000000000
```

すべてのファイルがキーフレームから始まるよう、`key-int-max` はセグメントの長さより短くしてください。NVR は、カメラ自身の圧縮ストリームを（再エンコードせずに）録画します。`amdva::streaming::SegmentRecorder`（`amdva/streaming/recorder.hpp`）は、その録画処理を再利用できるクラスにしたものです。キーフレームから始まる H.264/H.265 の access unit を `push()` で渡し、`poll()` を繰り返し呼ぶと、Matroska のセグメントを順次書き出しながら、`Catalog`（名前、メディアの開始時刻、長さ、中断したセグメント）を同期して更新します。保持期間と容量上限は、カタログとコントローラーが管理します（`examples/nvr/docs/vms.md`）。

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

ソースがすでにエンコード済み（ファイルやカメラの再配信）の場合は、デコードせずに配信してください。`amdva-rtsp-files` は H.264/H.265 のファイルに対してまさにこれを行います。ダッシュボード（`examples/resource-dashboard/README.md`）は GPU で描画したストリームを配信し、エンコーダーの出力に BT.709 の書き換えを適用します。

## SDK の部品

| ヘッダー | 用途 |
| --- | --- |
| `amdva/vui.hpp` | H.264 の access unit 向けの `signal_bt709()` |
| `amdva/rtsp_server.hpp` | `RtspServer`: マウント、appsrc ファクトリー、クライアント/セッションの終了処理 |
| `amdva/egl_import.hpp` | `EglDmaBufImporter`: VA サーフェスを EGLImage/GL テクスチャとしてエクスポート |
| `amdva/copy_audit.hpp` | コピー監査ツールの weak フック |

`AMDVA::amdva_media` をリンクしてください。
