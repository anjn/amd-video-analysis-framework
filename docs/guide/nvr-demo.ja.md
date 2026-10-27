# NVR デモ

NVR デモは、このフレームワークで作ったネットワークビデオレコーダーのサンプルです。登録したすべての RTSP カメラを録画しながら、最大 16 台をデスクトップに表示し、表示中のカメラで人物や車両の検出とトラッキングを行います。録画はタイムラインから再生でき、ネイティブの埋め込みモデルを用意すれば人物を文章や画像で検索することもできます。

## 画面構成

![NVR デモの画面構成（模式図）](images/nvr-ui-ja.svg)

| タブ | できること |
| --- | --- |
| Live | カメラを 1・4・9・16 分割のタイルに割り当てて表示します。検出枠とトラッキング ID を重ねて描画します |
| Cameras | カメラの追加・編集・削除、録画の保持期間の設定、録画の削除を行います |
| Search | 人物を文章、画像の領域、服装などの条件で検索します（別途セットアップが必要です） |
| AI Statistics | 推論の処理時間、検出の頻度、キューの状態を表示します |

タイルをダブルクリックすると 1 画面表示になり、左側の縦のタイムラインで録画を再生できます。

![1 画面表示と録画のタイムライン（模式図）](images/nvr-single-ja.svg)

タイムラインの緑の区間をクリックするとその時刻から再生し、マウスを重ねるとその時刻のプレビューを表示します。**Back to Live** でライブ表示に戻ります。表示していないカメラも録画は続けます。

## 起動する

[インストール](install.ja.md) を終え、`source scripts/env.sh` を実行したシェルで作業します。デスクトップ（Wayland または X11）と、AMD の GPU による OpenGL ES 3 が必要です。

### 1. モデルを準備する

[クイックスタート](quickstart.ja.md) と同じ手順で、バッチサイズ 4 のモデルも作ります。

```bash
mkdir -p artifacts/nvr
curl -fsSL -o artifacts/nvr/yolov8n.pt \
  https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8n.pt
PATH="$PWD/build/src/gst:$PATH" .deps/model-export/bin/python \
  examples/nvr/scripts/export_nvr_model.py artifacts/nvr/yolov8n.pt artifacts/nvr/nvr-model --batch=4
```

`artifacts/nvr/nvr-model/` に `yolov8n-b4.mxr`、`yolov8n-b1.mxr`、`model-info.json` ができれば成功です。

### 2. カメラを用意する

RTSP カメラがない場合は、付属の `amdva-rtsp-files` で動画ファイルをカメラの代わりに配信できます。ファイルごとに 1 つの URL を割り当て、ファイルの終わりまで来ると先頭から繰り返します。別のターミナルで実行し、デモを使っている間は動かしたままにします。

```bash
build/examples/rtsp-files/amdva-rtsp-files --port 8558 \
  artifacts/quickstart/bus.h264 artifacts/quickstart/out.mp4
```

`rtsp://127.0.0.1:8558/cam0` と `rtsp://127.0.0.1:8558/cam1` が表示されれば成功です。入力は H.264 または H.265 の、プログレッシブ、8 ビット、2560×1440 以下の映像です。

### 3. 設定ファイルを作る

次の内容で `artifacts/nvr/nvr-local.json` を作ります。パスは設定ファイルからの相対パスです。

```json
{
  "model": "nvr-model/yolov8n-b4.mxr",
  "single_model": "nvr-model/yolov8n-b1.mxr",
  "model_info": "nvr-model/model-info.json",
  "recording_root": "nvr-recordings",
  "sources": [
    {"id": "cam0", "name": "Camera 0", "uri": "rtsp://127.0.0.1:8558/cam0", "selected": true},
    {"id": "cam1", "name": "Camera 1", "uri": "rtsp://127.0.0.1:8558/cam1", "selected": true}
  ]
}
```

カメラは起動後に **Cameras** タブからも追加できます。変更は設定ファイルに保存されます。

### 4. 起動する

```bash
WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland-egl \
  build/examples/nvr/amdva-nvr artifacts/nvr/nvr-local.json
```

X11 のデスクトップでは `QT_QPA_PLATFORM=xcb` を指定します。ウィンドウが開き、2 台のカメラの映像に検出枠と ID が表示されれば成功です。録画は `artifacts/nvr/nvr-recordings/` に保存されます。

起動するたびに、そのディレクトリにある以前の録画は削除されます。残したい録画は、起動の前に別の場所へ移してください。

## 画面を使わずに動かす

`amdva-streams` は、同じ設定ファイルで受信、録画、検出だけを行います。ディスプレイがなくても動き、1 秒ごとに状態を JSON で出力します。

```bash
build/examples/nvr/amdva-streams artifacts/nvr/nvr-local.json 20
```

20 秒間動作し、最後の行が `{"live":2,"recording":2,...}` となっていれば成功です。

## 人物検索

人物検索には、EmbeddingGemma 2 または MobileCLIP2 のネイティブランタイムが別途必要です。検索結果を同じ人物ごとのカードにまとめるには、さらに ReID モデル（OSNet）を用意します。準備の手順と設定項目は [人物検索](../dev/nvr/nvr-search.ja.md) を参照してください。

## さらに詳しく

- 設定項目、処理の構成、検証結果: [NVR の開発者向け情報](../dev/nvr/README.ja.md)
- 画面の操作と録画の管理: [デスクトップの映像管理](../dev/nvr/vms.ja.md)
