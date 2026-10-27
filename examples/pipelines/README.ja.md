<!-- source: examples/pipelines/README.md 45b7da5b2d26e45b1a24d85c5c6a2e3f6dc0a0e0 -->
# gst-launch のレシピ

最初に `scripts/env.sh` を読み込んでください（パッケージをインストールした場合は、`GST_PLUGIN_PATH` にその `lib/gstreamer-1.0` を設定します）。どのスクリプトも入力を環境変数から読み、実行するコマンドを表示します。

| スクリプト | 内容 |
| --- | --- |
| `annotate-to-file.sh` | デコード、検出、描画を行い、ハードウェアエンコードで MP4/MKV に保存します |
| `annotate-segments.sh` | 同じストリームを `splitmuxsink` で一定の長さのファイルに分割します |
| `multistream-encode.sh` | 2 つの入力を 1 つの共有検出器（`gvastreammux`/`gvastreamdemux`）に通し、それぞれ個別に描画してエンコードします |

共通の環境変数は、`VIDEO`（H.264 エレメンタリストリームまたは MP4）、`MODEL`（`.mxr`）、`MODEL_INFO`（model-info JSON）、`CODEC`（`h264`、`h265`、`av1` のいずれか。既定は `h264`）、`BITRATE`（kbit/s。既定は 6000）です。

フレームは `vah264dec -> VAMemory NV12 -> amddetect -> amdwatermark
(BGRA DMA-BUF) -> amdencode (VA video processor -> VAMemory NV12 -> vah26xenc)` の順に流れます。画像を CPU にマップすることはありません。生の `.h264` ファイルにはタイムスタンプがないため、スクリプトは `video/x-h264,framerate=30/1` を付け加えます。タイミング情報を持つ MP4 や
RTSP の入力では、この capsfilter を外してください。
