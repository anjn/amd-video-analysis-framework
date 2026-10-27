<!-- source: docs/validation.md daf98fb5949c8f56bb01ce58a811458feed150be -->
# 検証と再現

当初の単一ストリームでの結果は `validation-results.json` に残しています。NVR の回帰試験には別のレポートがあります。拡張したフレームワークのタスク、結果の送信、Python、バッチ処理、GPU 描画、SDK の確認については、[フレームワークの検証](framework-validation.ja.md)と、その[記録した結果](framework-validation-results.json)で説明しています。ログ、コンパイル済みモデル、映像のテスト用素材は、無視対象の `artifacts/` に置いたままです。レポートには、すべての APU やモデルとの互換性を主張するのではなく、明記したホストで完了した確認を記録しています。

## ハードウェアとゼロコピーの根拠

`amdva-probe` は、既知の NV12 のテスト用素材を作成して VA でスケーリングと変換を行い、線形の BGRA の割り当てとしてエクスポートします。HIP でそれを読み取った後、VPE がその割り当てを別のテスト用素材で書き換えてから、**同じマップ済みポインター**を再び読み取ります。2 つのチェックサムが一致し、プロセスの VPE カウンターが増えている必要があります。CPU からのテスト用素材のアップロードと、スカラー値による検証用のダウンロードは、プローブで意図的に行う操作です。これらは分析用の映像パイプラインには含まれません。

`amdva-run` は、AMD の入力と最終出力で、元の GstMemory の同一性と PTS を確認します。プリロードするコピー監査ツールは、これとは別に、HIP の memcpy 操作、VA のイメージのダウンロード、アップロード、derive の呼び出し、VA イメージのメモリのマップを横取りします。従来の 7204 バイトの D2H 検出レコード、明示的に印を付けた上限のあるメタデータの転送、GPU 間での組み立てや所有権のためのコピーは許可します。印のないホストへの転送と、デコード済み画像のマップは拒否します。コンパイル済みの MIGraphX グラフについても、GPU への、または GPU からのコピー操作がないかを確認します。この根拠は、テストした公開 API の経路を対象とするものです。ドライバー内部のすべての操作を追跡したり、他の APU での互換性を確かめたりするものではありません。インポーターやドライバーが変わった場合は、エイリアスとパイプラインのプローブをやり直す必要があります。

報告している P95/P99 は、`amdpreproc` の入力から最終出力の sink
パッドまでの時間で、推論と DL Streamer の要素を含みます。ソースやデコードの遅延と、sink での表示待ちは含みません。全体の FPS には初期化が含まれます。そのため、短いファイルでの FPS は定常状態のスループットを表しません。

## 正しさと障害のテスト

```bash
source scripts/env.sh
cmake -S . -B build -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT" \
  -DAMDVA_HARDWARE_TESTS=ON -DAMDVA_DLSTREAMER_TESTS=ON
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

当初のベースラインのテストでは、次の項目を確認します。レイアウトの範囲、重なり、修飾子、テンソルの所有権と浅いコピー、PTS と元のメモリの同一性、厳密な caps、モデルがない場合や非 strict の場合の初期化の拒否、ハードウェアのエイリアスと VPE の実行、独立に求めた BT.709 の RGB 値とレターボックスの境界に対する実際の前処理、1 つの割り当てに対する 10000 回のインポート、マップ、解放のサイクル。前処理のリファレンステストでは、異なる 2 つのテスト用素材を同じ中間サーフェスに通し、保持していた最初のテンソルが上書きされないことを確認します。メタデータだけを扱う 3000 フレームの DL Streamer のストレステストでは、フレームごとに 6 件の検出がすべてそろうことと、500 フレームのウォームアップ後のピーク RSS の増加が 4 MiB 以下であることを求めます。このテストで、JSON の分類変換で文字列の取得が途中で失敗した場合のリークが明らかになりました。修正後の実装では、ローカルでの確認で増加量が 8 KiB でした。ヒーププロファイリングでも、分類ラベルのリークが残っている呼び出し履歴は見つかりませんでした。ハードウェアテストは明示的に有効にした場合だけ実行します。DL Streamer のテストはブートストラップで有効になります。

モデルの精度を確認するには、別途ダウンロードした CPU 版の ONNX Runtime 1.24.4 と、同じ入力テンソルを使います。ダンプツールは、オフラインでの検証のために意図的にそのテンソルをダウンロードします。この処理は監査対象のパイプラインの外にあります。

```bash
uv pip install --python .deps/venv/bin/python onnxruntime==1.24.4
build/src/gst/amdva-dump-tensor artifacts/bus.h264 artifacts/input.f32 \
  > artifacts/input-geometry.json
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/src/gst/amdva-run artifacts/bus.h264 artifacts/yolov8n.mxr \
  /path/yolov8n.json --events=artifacts/events.jsonl
.deps/venv/bin/python tools/compare_reference.py /path/yolov8n.onnx \
  artifacts/input.f32 artifacts/input-geometry.json artifacts/events.jsonl \
  /path/coco80.txt
```

リファレンスは、FP32 での CPU 推論、独立に実装したクラス別の NMS、座標の復元を行います。各検出について、クラスが同じであること、IoU が 0.90 以上であること、信頼度の差が 0.05 以下であることを求め、対応の取れない GPU の検出があってはなりません。テストしたフレームには 6 件の検出があり、すべて一致しました（IoU の最小値 0.9928、信頼度の差の最大値 0.00309）。これは再現可能な正しさの確認であり、データセット全体での精度評価ではありません。前処理は、別の合成データによるテストで検証します。

DL Streamer の JSON-lines の送信機能は、既存のファイルに追記します。実行ごとに新しいイベントのパスを使ってください。リファレンスの確認ツールは、あいまいな重複したタイムスタンプを拒否します。長時間試験のツールは、すでにイベントを含む出力ディレクトリを拒否します。

モデルの不一致、ランタイムエラー、対応していない caps は、GStreamer のバスを通じて失敗として通知されます。EOS、時間を指定したループの排出、プロセスの繰り返しの起動、正常な終了は、run と長時間試験の確認で検証しています。flush の動作は GstBaseTransform に従いますが、flush 中の障害注入と GPU ハングからの回復は検証していません。

## 長時間の運用

1 時間の長時間試験では、1080p30 の H.264 のテスト用素材を、時計に合わせたループで
AMD と DL Streamer の両方の要素に通します。RSS、FD の数、割り当て済みの VRAM/GTT を
5 秒ごとに取得します。GPU の割り当てカウンターは、fdinfo から DRM クライアントごとに 1 回集計します。重複した FD のエイリアスは除外し、カウンターが取得できない場合は検証失敗とします。60 秒のウォームアップの後、基準サンプルに対して、FD の増加のピークが 4 以下、RSS の増加のピークが
64 MiB 以下である必要があります。GPU の割り当ての増加のピークも 64 MiB 以下である必要があります。増加が大きすぎる場合は、ホストのメモリを使い切る前にテストを停止します。さらに、EOS の排出が成功すること、入力と出力の数が等しいこと、元のメモリと PTS の同一性、コピー監査の成功も求めます。これらの上限はこの PoC 用の回帰のしきい値であり、SDK 全般のメモリの目安ではありません。

ローカルで完了した 1 時間の実行では、107970 フレームを入力・出力し、647820 件を検出しました。同一性の違反と、観測されたコピーの違反はいずれも 0 件でした。全体の FPS は
29.9876、処理遅延の P95 は 10.412 ms、P99 は 10.4895 ms でした。ウォームアップ後の
RSS の増加のピークは 696 KiB、FD の増加のピークは 2 でした。割り当て済みの GPU メモリは
302540 から 308748 KiB の範囲で、ピークは基準サンプルを超えませんでした。EOS の排出も成功しました。すべての結果と環境は
[validation-results.json](validation-results.json) にあります。

```bash
python3 tools/soak.py artifacts/bus.h264 artifacts/yolov8n.mxr \
  /path/yolov8n.json --seconds=3600 --output=artifacts/soak
```

検証中に 4 つの問題を見つけて修正しました。テストした AMD の HIP インポーターは VA のエクスポートの FD を使用しておらず、デバイス I/O での MIGraphX の実行では、デフォルトストリームでの GPU の後処理の前に明示的に完了を待つ必要がありました。さらに、フレームごとに新しいサーフェスをインポートすると、ドライバーの GPU の割り当てが残り続けました。中間の VA サーフェスと HIP のマップを 1 つだけ保持することで、その割り当てを一定に抑えました。FD のストレステスト、2 フレームでの前処理、最初のフレームでの CPU のリファレンス、割り当て済みの GPU メモリの監視は、回帰の確認として残しています。RSS と FD の監視だけでは、GPU の割り当ての増加を見逃していました。DL Streamer の JSON の分類変換では、後続のモデル名の取得が失敗したときにラベルがリークしていました。両方の文字列を借用する形にして、途中で失敗する経路と例外の経路を修正しました。メタデータだけを扱う CPU のストレステストで、この組み込み部分の回帰を確認します。

## ローカルで使ったテスト用素材

検証には、背景となった既存プロジェクトにある YOLOv8n の ONNX とメタデータ、Ultralytics の bus の画像を使います。画像は FFmpeg/libx264 で 1920x1080 の H.264、30fps、limited レンジの BT.709 にエンコードします。テスト用素材では元の画像を 1920x1080 に引き伸ばし、前処理でそれを 640x640 にレターボックスします。リポジトリのソースでは、モデルや画像を再配布していません。合成した H.264 のテストパターンでもこの経路を検証します。

```bash
curl -fsSL https://raw.githubusercontent.com/ultralytics/ultralytics/main/ultralytics/assets/bus.jpg \
  -o artifacts/bus.jpg
ffmpeg -hide_banner -loglevel error -loop 1 -i artifacts/bus.jpg \
  -vf 'scale=1920:1080:out_color_matrix=bt709:out_range=limited' -t 1 -r 30 \
  -c:v libx264 -pix_fmt yuv420p -color_primaries bt709 -color_trc bt709 \
  -colorspace bt709 -f h264 artifacts/bus.h264
```

問題を調べる場合は、`GST_DEBUG=amdpreproc:6,amdmigraphx:6,gvatrack:5` のログ、依存関係のロックとキャッシュの検証結果、モデルのマニフェスト、プローブの JSON、ランナーの JSON、長時間試験の CSV を集めてください。ユーザー空間を固定しても、ホストの Mesa、ファームウェア、カーネルまでは固定されません。
