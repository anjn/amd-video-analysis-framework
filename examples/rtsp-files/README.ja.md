<!-- source: examples/rtsp-files/README.md 19b2aba0b7d74b5beedb3cd211feb5a01ba78614 -->
# 動画ファイルを RTSP カメラとして配信

`amdva-rtsp-files` は、入力ファイルごとに別々のカメラ URL で配信し、そのファイルを無限にループ再生します。エンコード済みの映像を少しずつ読み込み、映像のデコードもエンコードも行いません。この実行ファイルは Qt にも AMD GPU にも依存しません。同梱のランタイムでは、MP4/MOV と Matroska の H.264/H.265 映像に対応しています。音声、字幕、追加の映像トラックは破棄し、最初の対応している映像トラックを選択します。ファイルの一覧は起動時に固定されます。

## 実行

```bash
source scripts/env.sh
build/examples/rtsp-files/amdva-rtsp-files --list examples/rtsp-files/files.txt --port 8558
```

同梱の一覧は、生成された NVR のテスト用素材を参照しています。独自のファイルを使う場合は、1 行に 1 つのパスを書いた UTF-8 のテキストファイルを作成してください。

```text
# Paths relative to this list file, or absolute paths
/home/amd/Videos/entrance.mp4
/home/amd/Videos/parking.mkv
```

```bash
build/examples/rtsp-files/amdva-rtsp-files --list /path/to/videos.txt --port 8558
# Equivalent positional input:
build/examples/rtsp-files/amdva-rtsp-files --port 8558 "/path/to/video one.mp4" /path/to/video2.mkv
```

プログラムは、入力の順に次のような URL を標準出力に表示します。

```text
rtsp://127.0.0.1:8558/cam0
rtsp://127.0.0.1:8558/cam1
```

これらの URL を VMS の Cameras ページに追加するか、NVR の設定でソースの URI として使ってください。ポートは空いている必要があります。`--port` の既定値は 8554 です。ソースのファイル名、コーデック、解像度、検出した FPS は標準エラー出力にログとして出力されます。ファイルが存在しない場合、一覧が空の場合、映像が非対応の場合、ポートが使用中の場合は、0 以外の終了ステータスで終了します。

一覧には、空行、行全体の `#` コメント、空白を含むファイル名、UTF-8 BOM、CRLF の改行を含めることができます。一覧の各行は引用符で囲んだりエスケープしたりしないでください。シェルの引用符はコマンドライン引数にだけ適用されます。各行の先頭と末尾の空白は取り除かれます。パスは作業ディレクトリではなく、一覧ファイルのあるディレクトリからの相対パスです。同じファイルを複数行に書くと、そのファイルに独立したカメラ URL を複数割り当てられます。複数の `--list` 引数と位置引数のパスは、引数の順に組み合わせて指定できます。`--` でオプションの解析を終了します。

マウントの接頭辞を変更する場合や、別のマシンからの接続を許可する場合は、次のようにします。

```bash
build/examples/rtsp-files/amdva-rtsp-files --list videos.txt \
  --address 0.0.0.0 --port 8558 --prefix /video
```

この例では `/video0`、`/video1` などが作られます。リモートのクライアントでは、サーバーの LAN アドレスを使ってください。既定の待ち受けアドレスは `127.0.0.1` です。RTSP over TCP と UDP の両方を使えます。認証や TLS はありません。これはローカルや LAN でのデモ用の入力ツールです。Ctrl+C または SIGTERM でサーバーが停止し、クライアントとの接続を閉じます。

## タイミングとリソース使用量

コンテナーの PTS/DTS とフレームの長さは、ストリームの先頭を基準として保持されます。ループのたびに RTP の時刻をリセットするのではなく、メディアのタイムラインを進めます。ループの範囲は表示タイムスタンプとデコードタイムスタンプの両方の範囲を含みます。edit list や並べ替えのためのパディングによって、ループの境目で一時的に停止することがあります。B フレームの表示順は保たれます。コンテナーに DTS がない場合（Matroska でよくあります）は、AU の長さから別のデコード用クロックを合成します。可変のフレームタイミングがあればそのまま保持し、すべての入力を 30 fps に揃えることはしません。

Annex-B 形式の生の `.h264`/`.h265` ストリームも使えます。生のエレメンタリストリームのファイルはコンテナーの表示タイミングを持たないため、B フレームを含む映像には MP4/MKV を使ってください。タイムスタンプのない生ファイルでは、パーサーが報告する FPS を使い、FPS が不明な場合は
`--fallback-fps N`（既定値は 30）を使います。表示タイミングのない生ストリームには
B フレームが含まれていてはいけません。ファイルはデコード可能なキーフレームから始まっている必要があります。このツールは、NVR の入力制限に合わせて内容をリサイズしたりトランスコードしたりしません。また、配信中に入力ファイルを変更してはいけません。

接続された各 URL には、そのクライアント間で共有されるプロデューサーが 1 つあります。動作中の URL に新しく接続したクライアントは、その時点の再生位置から参加し、キーフレームを待つことがあります。URL が異なれば、ファイル名が同じでもプロデューサーは独立しています。使われていない URL では、映像を読み込み続けることはありません。事前確認と再送出では、ファイル全体を保持せず、上限付きの appsink/queue のバッファーリングを使います。ただし、コンテナーのメタデータと少数の圧縮済みアクセスユニットはメモリを消費します。ファイルはループの境目で開き直します。時刻でシークできない生の入力も同様です。

## ビルドと検証

通常のプロジェクトでは `AMDVA_BUILD_RTSP_FILES=ON` が有効です。このターゲットは
`AMDVA_BUILD_NVR`、`AMDVA_BUILD_STREAMING`、ダッシュボードとは独立しています。ブートストラップでは、既存の Matroska 対応に加えて、MP4/MOV 用の `isomp4` プラグインを有効にします。C++17 コンパイラーと GStreamer 1.28.2 の開発用ファイル（core、app、RTSP server）があれば、このツールだけをビルドすることもできます。

```bash
source scripts/env.sh
cmake -S examples/rtsp-files -B artifacts/rtsp-files-only
cmake --build artifacts/rtsp-files-only -j4
ctest --test-dir artifacts/rtsp-files-only --output-on-failure
```

ランタイムには、`parsebin`、`filesrc`、`appsink`、`appsrc`、`identity`、`fakesink`、H.264/H.265 のパーサーと RTP ペイローダー、および該当するデマルチプレクサー（`qtdemux` または `matroskademux`）が必要です。非対応のコーデックは、黙ってトランスコードするのではなく拒否します。

生成した 2 秒間のテスト用素材を使って機能テストを行うには、次のようにします。

```bash
source scripts/env.sh
/usr/bin/python3 examples/rtsp-files/validate.py \
  --server build/examples/rtsp-files/amdva-rtsp-files \
  --output artifacts/rtsp-files-check --hardware-decode
```

これには、libx264/libx265/AAC でエンコードできる FFmpeg/FFprobe、Python GI/GStreamer、`--hardware-decode` 用の VA デコーダーが必要です。GPU のないマシンでエンコード済みデータだけを受け取るクライアントでテストする場合は、このフラグを省いてください。テストの対象は、音声付きの MP4、Matroska、生の映像、B フレーム、25/29.97/24 fps と可変のフレーム間隔、同じパスの繰り返し、UTF-8 のパスと一覧ファイルからの相対パス、ループ再生、クライアントの共有、TCP/UDP、再接続、不正な入力、ポートの競合、終了処理です。テストでは、デコードしたフレーム数と受信したフレーム数、タイミング、およびサーバーの RSS と
FD 数のピークを記録します。短時間の実行では、長期間のリソースの安定性は確認できません。[検証記録](validation-results.json)を参照してください。
