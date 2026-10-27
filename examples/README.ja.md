<!-- source: examples/README.md 6f99a162a0eef787fdf4150e16745c1fb614c283 -->
# サンプル

フレームワークを使って作成したアプリケーションです。インストールされるヘッダーと
`AMDVA::` の CMake ターゲットだけを使うため、各ディレクトリはインストール済みの SDK を使って単独で
configure することもできます（`cmake -S examples/<name> -B build-<name> -DCMAKE_PREFIX_PATH=/path/to/amdva-sdk`）。ソースツリー内では、`-DAMDVA_BUILD_EXAMPLES=OFF` を指定しない限りフレームワークと一緒にビルドします。

| サンプル | 内容 |
| --- | --- |
| [NVR](nvr/README.ja.md) | 16 カメラのレコーダー。Qt に依存しないコントローラー、人物検索、ヘッドレスの `amdva-streams` サービス、Qt デスクトップ |
| [リソースダッシュボード](resource-dashboard/README.ja.md) | CPU/GPU/メモリ/電力の計測値を、GPU で描画してハードウェアエンコードする RTSP ダッシュボード |
| [ファイルを RTSP で配信するサーバー](rtsp-files/README.ja.md) | テスト用に MP4/MKV ファイルをループ再生の RTSP カメラとして配信します |
| [SDK のプログラム](sdk) | トランスコード、デコード/表示のテレメトリ、録画の再生、タイムラインとキーフレームのプレビュー |
| [gst-launch のレシピ](pipelines/README.ja.md) | 描画してファイルに保存、セグメントの録画、マルチストリームのエンコード |
| [`yolov8n.json`](yolov8n.json)、[`metadata_callback.py`](metadata_callback.py) | YOLOv8n 用の model-info と `amdpython` のコールバック |
