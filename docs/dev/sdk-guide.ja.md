<!-- source: docs/dev/sdk-guide.md 8642f1b6998decc453889b46c6c1e74beb1866bc -->
# SDK ガイド

## ライブラリ

| ターゲット | ヘッダー | 内容 |
| --- | --- | --- |
| `AMDVA::amdva_runtime` | `runtime.hpp`、`preprocess.hpp`、`telemetry.hpp` | VA/HIP のインポート（`VaSession`、`ImportedSurface`）、VPE 処理、画像のパッキング |
| `AMDVA::amdva_inference` | `model.hpp`、`batching.hpp`、`inference.hpp` | `ModelInfo`、`ModelExecutor`、タスクごとの後処理器、共有バッチの実行器 |
| `AMDVA::amdva_gst_support` | `tensor_meta.hpp`、`video_preprocess.hpp`、`tracking.hpp`、`overlay.hpp`、`publishing.hpp`、`gst_telemetry.hpp` | GstAnalytics との相互運用、トラッキング、描画、結果の送信 |
| `AMDVA::amdva_media` | `vui.hpp`、`rtsp_server.hpp`、`egl_import.hpp`、`copy_audit.hpp` | エンコードの補助機能、RTSP 配信、EGL インポート |
| `AMDVA::amdva_streaming` | `streaming/*.hpp` | `RtspIngest`、`SegmentRecorder`、`Catalog`（録画、録画の解析結果、アプリケーションのテーブル）、再生セッション、`PreviewSlot` |
| `AMDVA::amdva_monitor` | `resource_monitor.hpp` | CPU/GPU/メモリ/電力と DRM エンジンの負荷 |

共有ライブラリの SONAME は `lib<name>.so.<major>.<minor>` です。0.x のマイナーリリースでは
ABI の互換性を保証しないため、マイナーバージョンが変わったらアプリケーションを再ビルドしてください。

プラグイン（`gstamdanalytics`、`gstgva`、`gstgvametapublish*`）は
`lib/gstreamer-1.0` にインストールされます。システム以外のプレフィックスを使う場合は、そのディレクトリを `GST_PLUGIN_PATH` に追加してください。

## ビルドとインストール {#build-and-install}

```bash
source scripts/env.sh                 # rootless dependency environment
cmake -S . -B build -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8
cmake --install build --prefix /path/to/amdva-sdk
```

主なオプションは、`AMDVA_BUILD_PLUGINS`、`AMDVA_BUILD_GVA_COMPAT`（同梱の DL
Streamer 要素）、`AMDVA_BUILD_STREAMING`、`AMDVA_HARDWARE_TESTS` です。`examples/` 以下のアプリケーション向けには、`AMDVA_BUILD_EXAMPLES`、`AMDVA_BUILD_NVR`、`AMDVA_BUILD_NVR_GUI`（Qt6 が必要）、`AMDVA_BUILD_DASHBOARD`、`AMDVA_BUILD_RTSP_FILES` があります。

省略可能な依存関係は、いずれも既定で `ON` です。結果の送信用の `AMDVA_WITH_MQTT`（Eclipse Paho）と
`AMDVA_WITH_KAFKA`（librdkafka）、`amdpython` 用の `AMDVA_WITH_PYTHON`（組み込み CPython）があります。フレームワークを最小構成でビルドする例を示します。

```bash
cmake -S . -B build-min -DAMDVA_ROCM_ROOT="$AMDVA_ROCM_ROOT" -DAMDVA_BUILD_EXAMPLES=OFF \
  -DAMDVA_WITH_MQTT=OFF -DAMDVA_WITH_KAFKA=OFF -DAMDVA_WITH_PYTHON=OFF
```

`AMDVA_SYSROOT`（既定は `.deps/sysroot`）は、root 権限なしで導入した開発用パッケージの場所を指定します。インストールしたパッケージには `amdva_use_host_glib()` と
`amdva_use_host_sqlite()`（`AmdvaHostLibraries.cmake`）が含まれ、アプリケーションはフレームワークと同じようにホストの GLib/SQLite ランタイムをリンクできます。

## CMake からの利用

```cmake
find_package(AMDVideoAnalysis REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE AMDVA::amdva_media)
```

`examples/sdk/CMakeLists.txt` は単独のプロジェクトです。

```bash
cmake -S examples/sdk -B build-sdk -DCMAKE_PREFIX_PATH=/path/to/amdva-sdk
```

## サンプル

| プログラム/スクリプト | 内容 |
| --- | --- |
| `examples/sdk/transcode.cpp` | デコード、検出、オーバーレイを行い、ファイルまたは RTSP にエンコード |
| `examples/nvr/headless.cpp` | サンプルのコントローラーを使うヘッドレスの NVR サービス（RTSP の受信、録画、AI） |
| `examples/nvr`、`examples/resource-dashboard`、`examples/rtsp-files` | 完成したアプリケーション。各 README を参照 |
| `examples/sdk/playback.cpp`、`timeline.cpp`、`keyframe-preview.cpp` | 録画映像の API |
| `examples/sdk/telemetry.cpp` | デコードと表示のタイミング |
| `examples/pipelines/*.sh` | `gst-launch` のレシピ。描画してファイルに保存、セグメント分割、マルチストリーム |
| `examples/metadata_callback.py` | `amdpython` のコールバック |

## テスト

```bash
ctest --test-dir build                        # no GPU needed
cmake -S . -B build -DAMDVA_HARDWARE_TESTS=ON \
  -DAMDVA_DETECTION_MODELS=/dir/with/yolov8n-b1.mxr+model-info.json \
  -DAMDVA_TEST_VIDEO=/path/test.h264
ctest --test-dir build -R "encode|framework"  # needs the AMD GPU
```

ハードウェアテストには、`encode-contract`（H.264/H.265/AV1、NV12 と BGRA の入力、BT.709 のシグナリング）と `encode-annotated-contract`（DMA-BUF の経路でのデコード、検出、オーバーレイ、エンコード）が含まれます。

## 依存関係とライセンス

`scripts/bootstrap.py` は、GstAnalytics パッチを当てたバージョン固定の GStreamer 1.28.2 と、root 権限を使わない sysroot を `.deps/` 以下にビルドします。DL Streamer のソースは
`third_party/dlstreamer`（MIT）に同梱しています。`THIRD_PARTY_LICENSES.md` を参照してください。
