# リソースダッシュボード

[English](README.md)

APU の CPU、GPU、メモリ、電力、動画エンジンの使用率を GPU で描画し、ハードウェアエンコードした H.264 を RTSP で配信するサンプルです。ブラウザー、Qt、推論モデルは使いません。

```bash
source scripts/env.sh
build/examples/resource-dashboard/amdva-dashboard --power-max-w 48
```

`Dashboard ready: rtsp://...` と表示されたら、`rtsp://<この PC の IP>:8554/dashboard` を VLC などの RTSP プレーヤーや NVR デモのカメラとして開きます。認証はないため、この PC の中だけで使う場合は `--bind 127.0.0.1` を指定してください。`--power-max-w` は電力バーの表示上の上限（W）です。

画面のデザインは [index.html](index.html) をブラウザーで開くと、模擬データで確認できます。オプション、計測値の取得元、検証結果は [リソースダッシュボードの開発者向け情報](../../docs/dev/resource-dashboard/README.ja.md) を参照してください。
