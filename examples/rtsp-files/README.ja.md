# ファイル RTSP サーバー

[English](README.md)

動画ファイルを RTSP カメラの代わりに配信するテスト用のツールです。ファイルごとに 1 つの URL を割り当て、終わりまで来ると先頭から繰り返します。映像はデコードも再エンコードもしないため、GPU は使いません。

```bash
source scripts/env.sh
build/examples/rtsp-files/amdva-rtsp-files --port 8558 /path/to/a.mp4 /path/to/b.mkv
```

`rtsp://127.0.0.1:8558/cam0`、`rtsp://127.0.0.1:8558/cam1` のように、入力の順に URL が表示されます。対応する入力は MP4/MOV と Matroska の H.264/H.265、および H.264/H.265 の生ストリームです。ファイルの一覧を [files.txt](files.txt) のようなテキストファイルで渡す場合は `--list` を使います。オプションと検証結果は [ファイル RTSP サーバーの開発者向け情報](../../docs/dev/rtsp-files/README.ja.md) を参照してください。
