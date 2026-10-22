#include "window.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace amdva::nvr;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    auto dir =
        std::filesystem::temp_directory_path() / ("amdva-config-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto path = dir / "config.json";
    auto require = [](bool value, const char *text) {
        if (!value)
            throw std::runtime_error(text);
    };
    auto write = [&](QJsonObject j) {
        QFile f(QString::fromStdString(path));
        f.open(QIODevice::WriteOnly);
        f.write(QJsonDocument(j).toJson());
    };
    QJsonObject base{{"model", "batch.mxr"},
                     {"single_model", "single.mxr"},
                     {"model_info", "model.json"},
                     {"sources", QJsonArray{QJsonObject{{"id", "a"},
                                                        {"uri", "rtsp://user:secret@localhost/a"},
                                                        {"selected", true}}}}};
    try {
        write(base);
        auto o = load_config(path);
        require(o.tracking_enabled && o.results.method == "none", "Analytics defaults");
        require(o.telemetry.enabled && o.telemetry.stall_ms == 1000, "Telemetry defaults");
        require(o.retention_minutes == 60, "Retention default");
        require(!o.search.enabled && o.search.image_size == 384 && o.search.queue_limit == 64,
                "Search defaults");
        {
            auto value = base;
            value["search"] = QJsonObject{{"enabled", true},
                                          {"bundle", "bundle"},
                                          {"image_size", 384},
                                          {"reid_model", "osnet.mxr"}};
            write(value);
            auto search = load_config(path);
            require(search.search.enabled && search.background_ai_enabled &&
                        search.search.bundle == (dir / "bundle"),
                    "Search config and hidden cameras");
            save_config(path, search);
            require(load_config(path).search.bundle == search.search.bundle &&
                        load_config(path).search.reid_model == (dir / "osnet.mxr"),
                    "Search and ReID roundtrip");
            for (int size : {0, 31, 1025}) {
                value["search"] = QJsonObject{{"image_size", size}};
                write(value);
                bool rejected = false;
                try {
                    load_config(path);
                } catch (...) {
                    rejected = true;
                }
                require(rejected, "Search image size validation");
            }
            write(base);
        }
        o.retention_minutes = 0;
        o.telemetry.enabled = false;
        o.telemetry.stall_ms = 1200;
        require(o.batch == 4 && o.segment_seconds == 60, "Defaults");
        require(!o.background_ai_enabled && o.background_ai_interval_seconds == 5,
                "Background defaults");
        o.tracking_enabled = false;
        o.results.method = "file";
        o.results.file_path = (dir / "events.jsonl").string();
        o.results.queue_limit = 9;
        o.results.byte_limit = 2048;
        o.background_ai_enabled = true;
        o.background_ai_interval_seconds = 3;
        require(o.model == (dir / "batch.mxr"), "Relative model path");
        require(o.recording_root == (dir / "recordings"), "Relative recording root");
        save_config(path, o);
        struct stat st {};
        stat(path.c_str(), &st);
        require((st.st_mode & 0777) == 0600, "Credential file permissions");
        require(load_config(path).retention_minutes == 0, "Retention disable persistence");
        for (double invalid : {-1., 1.5, 2147483648.}) {
            auto value = base;
            value["retention_minutes"] = invalid;
            write(value);
            bool rejected = false;
            try {
                load_config(path);
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Invalid retention accepted");
        }
        save_config(path, o);
        require(!load_config(path).telemetry.enabled &&
                    load_config(path).telemetry.stall_ms == 1200,
                "Telemetry persistence");
        require(!load_config(path).tracking_enabled && load_config(path).results.queue_limit == 9 &&
                    load_config(path).results.byte_limit == 2048,
                "Analytics config persistence");
        require(load_config(path).sources[0].uri == o.sources[0].uri, "Credential persistence");
        require(load_config(path).background_ai_enabled &&
                    load_config(path).background_ai_interval_seconds == 3,
                "Background persistence");
        for (const auto &item : std::vector<std::pair<QString, QJsonValue>>{
                 {"quota_gib", -1},
                 {"quota_gib", "bad"},
                 {"reserve_gib", -1},
                 {"segment_seconds", 0.5},
                 {"batch_size", 3},
                 {"sources", "bad"},
                 {"tracking_enabled", "true"},
                 {"telemetry", "bad"},
                 {"telemetry", QJsonObject{{"enabled", "false"}}},
                 {"telemetry", QJsonObject{{"stall_ms", 0}}},
                 {"telemetry", QJsonObject{{"stall_ms", 1.5}}},
                 {"telemetry", QJsonObject{{"stall_ms", 3600001}}},
                 {"results", "bad"},
                 {"results", QJsonObject{{"method", "unknown"}}},
                 {"results", QJsonObject{{"queue_limit", 0}}},
                 {"results", QJsonObject{{"byte_limit", -1}}},
                 {"background_ai_enabled", "true"},
                 {"background_ai_interval_seconds", 0},
                 {"background_ai_interval_seconds", 3601},
                 {"background_ai_interval_seconds", 1.5}}) {
            auto j = base;
            j[item.first] = item.second;
            write(j);
            bool reject = false;
            try {
                load_config(path);
            } catch (...) {
                reject = true;
            }
            require(reject, "Invalid config accepted");
        }
        auto upper = base;
        auto camera = upper["sources"].toArray()[0].toObject();
        camera["uri"] = "RTSP://user:secret@localhost/a";
        upper["sources"] = QJsonArray{camera};
        write(upper);
        require(load_config(path).sources[0].uri == o.sources[0].uri, "URI scheme normalization");
        auto j = base;
        auto sources = j["sources"].toArray();
        sources.append(sources[0]);
        j["sources"] = sources;
        write(j);
        bool reject = false;
        try {
            load_config(path);
        } catch (...) {
            reject = true;
        }
        require(reject, "Duplicate IDs accepted");
        j = base;
        auto s = j["sources"].toArray()[0].toObject();
        s["id"] = "../escape";
        j["sources"] = QJsonArray{s};
        write(j);
        reject = false;
        try {
            load_config(path);
        } catch (...) {
            reject = true;
        }
        require(reject, "Unsafe ID accepted");
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        std::filesystem::remove_all(dir);
        return 1;
    }
    std::filesystem::remove_all(dir);
    std::cout
        << "Defaults, relative paths, atomic owner-only save and invalid config rejection passed\n";
}
