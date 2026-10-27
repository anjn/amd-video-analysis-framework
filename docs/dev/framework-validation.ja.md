<!-- source: docs/dev/framework-validation.md e7f73478df2299e40382235de5c22799c2d3e142 -->
# フレームワークの検証

フレームワークのテストは、当初の単一ストリームと NVR の確認に、タスクの検証範囲を追加します。レポートは、検証した gfx1150 / ROCm 7.2 / Mesa 25.2.8 のホストに当てはまるもので、すべての AMD APU やすべての ONNX モデルに当てはまるわけではありません。記録した結果は
[framework-validation-results.json](framework-validation-results.json) にあります。モデルの重み、コンパイル済みのバイナリ、画像、フレームごとのログは、無視対象の `artifacts/` に置いたままです。

## 記録した実行

記録した実行では、14 件の CTest の確認、監査付きの完全なパイプライン、5 件の独立した CPU でのモデル比較がすべて成功しました。3,600 秒の NVR の実行では、20 の RTSP
ソース、16 の選択中のビュー、4 つのバックグラウンドの枠を使い、動的な操作を 100
サイクル行い、サーバーの再起動とコーデックの差し替えも行いました。確定した 1,307 個の MKV ファイルはすべて
FFprobe の確認に合格し、回復には 7.0 秒かかり、コピーの違反は 0 件のままでした。ウォームアップ後の最大の増加量は、RSS が 23,144 KiB、GPU の割り当てが 34,656 KiB、FD が 2 個でした。選択中のソースごとの表示と AI のレートの中央値は 29.883 FPS と 5.966 FPS で、記録したベースラインの 99.999% と 99.805% にあたり、受け入れしきい値の 90% を上回りました。この実行の一部の時間帯には他の検証の負荷が重なっていたため、これらのレートは他の負荷から切り離したスループットのベンチマークではなく、観測値です。

1 時間の実行では、最終的なストリームの flush、オーバーレイのラベル、検証の強化に関する変更より前に固定したランタイムを使いました。そのバイナリのハッシュはレポートに記録しています。最終ビルドでは別途、再起動、コーデックの差し替え、動的な操作、トラッキング、ファイルへの結果の送信を含む 90 秒の NVR の回帰試験に合格しました。インストールした DL Streamer のパッチを使った、すべての契約テストと監査付きのパイプラインのテストにも合格しました。ヘッドレスのビルドは、Qt やディスプレイを使わずに実際の 2 つの RTSP ソースを処理し、外部のアプリケーションはインストールした SDK に対してビルドと実行ができました。

## 契約テスト

```bash
source scripts/env.sh
python3 scripts/setup_model_export.py
.deps/model-export/bin/python tools/prepare_test_models.py artifacts/test-models
cmake -S . -B build -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT" \
  -DAMDVA_HARDWARE_TESTS=ON -DAMDVA_DLSTREAMER_TESTS=ON \
  -DAMDVA_TEST_MODELS="$PWD/artifacts/test-models"
cmake --build build -j6
ctest --test-dir build --output-on-failure
```

CPU と API のテストでは、厳密な caps、ソース、設定、カタログの動作、メタデータの所有権、Python での pass、drop、custom、例外、型、サイズの扱い、結果の送信のキューの上限、送信先の回復を確認します。結果の送信のテストでは、ループバックの MQTT
プロトコルのテスト用素材と、librdkafka のローカルのモッククラスターを使います。外部のブローカーには接続しません。QoS1 の確認応答と配信のコールバックは検証しますが、本番環境のブローカーの認証や TLS の設定、exactly-once の動作は検証しません。

GPU のテストでは、次の項目を確認します。名前付きの複数入力・複数出力の実行と、別の評価やモデルの破棄の後も出力が保持されること。バッチサイズ 1/2/4/8/16、共有、一部だけの待機、キャンセル、キューのあふれ。分類の softmax と top-k。クラス別の NMS。セマンティックの argmax。インスタンスのプロトタイプからの再構成。キーポイント。ROI の逆変換と NV12 のアラインメント。複数段の VPE による BGR/mean/std のリファレンス。メタデータの統合での ID の付け替えと GPU のペイロードの寿命。実際のオーバーレイのピクセル。許可される呼び出しと禁止される呼び出しのコピー監査。

## 代表的なモデルと完全なパイプライン

[フレームワークの使い方](framework.ja.md)で説明しているモデルをエクスポートしてコンパイルします。1920x1080 の H.264 のテスト用素材と、同じ重みから作った NVR 用の YOLOv8n の B1/B4 の成果物を用意してください。異種の mux のテスト用に、2 本目の小さいストリームを作成します。

```bash
ffmpeg -i "$VIDEO" -frames:v 1 -vf scale=640:360 -c:v libx264 \
  -pix_fmt yuv420p -color_primaries bt709 -color_trc bt709 -colorspace bt709 \
  "$MODELS/small.h264"
build/src/gst/framework-pipeline-tests "$VIDEO" "$MODELS" "$NVR_MODELS"
```

パイプラインのテストでは、検出 → トラッキング → ROI の分類 → Python →
GPU のウォーターマーク、姿勢推定、セマンティックセグメンテーション、tee とメタデータの集約を使ったインスタンスセグメンテーションの分岐、異種の mux → B4 の推論 → demux を確認します。描画の前の元の画像の GstMemory と PTS、AMD の添付データの保持、ROI の親へのリンク、Python のカスタムメタデータ、mux の両方のソース、パイプラインの破棄後の出力の寿命を検証します。flush とストリームの差し替えのテストでは、独立した
VA ディスプレイと解像度を切り替え、新しいソースの識別情報、世代、PTS、元の画像のメモリを検証します。demux の各分岐には専用のキューを置き、sink のプリロールがもう一方のソースを止めないようにしています。パスは GStreamer のパーサー向けに引用符で囲みます。

CTest でこのテストを有効にするには、`AMDVA_ANALYTICS_MODELS`、`AMDVA_DETECTION_MODELS`、`AMDVA_TEST_VIDEO` を設定します。ウォームアップの後、同じパイプラインのテストをコピー監査ツール付きで実行します。

```bash
LD_PRELOAD="$PWD/build/src/gst/libamdva_copy_audit.so" \
  build/src/gst/framework-pipeline-tests "$VIDEO" "$MODELS" "$NVR_MODELS"
```

このテストは、最初のフレームの処理が完了した後に監査を有効にします。これにより、モデルやカーネルの初期化を対象から外しつつ、その後の推論、ROI の処理、トラッキング、メタデータ、描画、出力を対象に含めます。コピー監査ツールのカウンターは、すべてのパイプラインで違反 0 件のままである必要があります。

オフラインの検証ツールは、独立した ONNX Runtime の CPU での比較のために、用意したテンソルとモデルの出力を意図的にダウンロードします。

```bash
for name in resnet18 yolov8n-seg yolov8n-pose segformer-b0; do
  build/src/gst/amdva-validate-model "$VIDEO" "$MODELS/$name.mxr" \
    "$MODELS/$name.json" "$MODELS/$name-reference"
done
build/src/gst/amdva-validate-model "$VIDEO" "$NVR_MODELS/yolov8n-b1.mxr" \
  "$NVR_MODELS/model-info.json" "$MODELS/yolov8n-reference"
.deps/model-export/bin/python tools/validate_models.py "$MODELS" "$NVR_MODELS" \
  --output="$MODELS/reference-results.json"
```

CPU 側のモデルには、VPE と HIP で用意したテンソルをそのまま入力するので、推論の誤差をスケーラーやフィルターの違いから切り分けられます。独立した単色の BT.709 の前処理のリファレンスで、スケーリング、クロマに合わせた切り出し、BGR の順序、正規化を別に検証します。FP16 の生の出力は、相対 L2 誤差が 1% 未満である必要があります。分類の top-k は一致する必要があります。信頼度の高い物体は、クラス、信頼度、IoU が一致する必要があります。セマンティックのピクセルの一致率は 99% を超える必要があります。インスタンスのマスクの IoU は
.95 を超える必要があります。信頼度の高い姿勢推定のキーポイントは、元の画像で 5 ピクセル以内に収まる必要があります。これらはテスト用素材での正しさの確認であり、ImageNet、COCO、ADE20K のデータセットでの精度のベンチマークではありません。

## NVR と SDK の回帰試験

当初の NVR の長時間試験を、録画する 20 ソース、16 の選択中のソース、4 つのバックグラウンドの枠、追加、削除、選択の 100 サイクル、600 秒時点でのサーバーの再起動、コーデックの差し替えで実行します。`--build-dir` には固定したビルドツリーを指定できるので、開発作業によって 1 時間の実行で使うライブラリが置き換わることを防げます。その実行の `LD_LIBRARY_PATH` ではそのツリーを優先します。実行中は依存関係も変更しないでください。リソースの監視の条件は、ウォームアップ後の RSS と GPU の増加が 64 MiB 以内であることと、既存のチャンネルごとの FD の上限のままです。FFprobe で確定済みの MKV ファイルを検証し、コピー監査ツールが報告する違反は 0 件である必要があります。2 回目の実行ではファイルへの結果の送信を有効にし、選択中のソースのトラッキング付きの観測、トラッキングなしのバックグラウンドの結果、ソースの識別情報を確認します。

パッケージは、一時的なプレフィックスにインストールし、`AMDVideoAnalytics_DIR` をそのプレフィックスに設定して
`examples/sdk` をビルドすることで検証します。これとは別に、`AMDVA_BUILD_NVR_GUI=OFF` のビルドが Qt なしで成功し、その `amdva-streams`
実行ファイルがディスプレイサーバーなしで実際の RTSP ソースを処理できる必要があります。テストのレポートには、これらの結果を GUI での表示の根拠とは分けて記録しています。


テレメトリ固有の境界、手動の検証ツール、記録したハードウェアでの結果については、[デコードと表示のテレメトリ](telemetry.ja.md)で説明しています。その[検証レポート](telemetry-validation-results.json)では、長時間の実行に使った固定のランタイムと、最終版での SDK、レンダラー、RTP、テレメトリの有効と無効の確認を分けて記録しています。
