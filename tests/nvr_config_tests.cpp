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
        require(o.batch == 4 && o.segment_seconds == 60, "Defaults");
        require(o.model == (dir / "batch.mxr"), "Relative model path");
        require(o.recording_root == (dir / "recordings"), "Relative recording root");
        save_config(path, o);
        struct stat st {};
        stat(path.c_str(), &st);
        require((st.st_mode & 0777) == 0600, "Credential file permissions");
        require(load_config(path).sources[0].uri == o.sources[0].uri, "Credential persistence");
        for (const auto &item :
             std::vector<std::pair<QString, QJsonValue>>{{"quota_gib", -1},
                                                         {"quota_gib", "bad"},
                                                         {"reserve_gib", -1},
                                                         {"segment_seconds", 0.5},
                                                         {"batch_size", 3},
                                                         {"sources", "bad"}}) {
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
