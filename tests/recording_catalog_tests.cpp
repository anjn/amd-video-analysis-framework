#include "amdva/streaming/catalog.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>
using amdva::streaming::Catalog;
using amdva::streaming::RecordingQuery;
static void require(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
int main() {
    auto root =
        std::filesystem::temp_directory_path() / ("amdva-catalog-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        Catalog c(root);
        std::ofstream(root / "external.mkv") << std::string(100, 'x');
        c.opened("old.mkv", "a");
        std::ofstream(root / "old.mkv") << std::string(100, 'a');
        c.closed("old.mkv");
        c.opened("active.mkv", "b");
        std::ofstream(root / "active.mkv") << std::string(100, 'b');
        c.opened("link.mkv", "c");
        std::filesystem::create_symlink(root / "external.mkv", root / "link.mkv");
        c.closed("link.mkv");
        require(c.bytes() == 200, "Catalog byte accounting");
        require(c.enforce(100, 0), "Cannot prune closed recording");
        require(!std::filesystem::exists(root / "old.mkv"), "Old recording retained");
        require(std::filesystem::exists(root / "active.mkv"), "Active recording deleted");
        require(std::filesystem::exists(root / "external.mkv"), "External recording deleted");
        require(std::filesystem::is_symlink(root / "link.mkv"), "Symlink deleted");
        require(!c.enforce(1, 0), "Active-only quota should fail");
        bool rejected = false;
        try {
            c.opened("../outside.mkv", "x");
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Traversal accepted");
        rejected = false;
        try {
            Catalog second(root);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Concurrent writer accepted");
        c.closed("active.mkv");
        require(c.enforce(1, 0), "Finalized recording not eligible");
        c.opened("pinned.mkv", "a", "Entrance", "session-a", 3);
        std::ofstream(root / "pinned.mkv") << std::string(100, 'p');
        c.closed("pinned.mkv", 2000000000LL);
        auto pin = c.open_recording("pinned.mkv");
        require(pin->fd() >= 0 && pin->recording().camera_name == "Entrance",
                "Playback handle metadata");
        require(!c.enforce(1, 0) && std::filesystem::exists(root / "pinned.mkv"),
                "Playing recording pruned");
        pin.reset();
        require(c.enforce(1, 0), "Released recording not pruned");
        c.opened("interrupted.mkv", "deleted-camera");
        c.interrupted("interrupted.mkv");
        RecordingQuery query;
        query.channel = "deleted-camera";
        auto result = c.list(query);
        require(result.size() == 1 && result[0].state == "interrupted" && !result[0].available,
                "Interrupted/missing state");
        rejected = false;
        try {
            c.open_recording("interrupted.mkv");
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Interrupted recording opened");
        for (unsigned i = 0; i < 4; i++)
            c.opened("page" + std::to_string(i) + ".mkv", "pages");
        query = {};
        query.channel = "pages";
        query.limit = 2;
        auto page = c.list(query);
        require(page.size() == 2, "Page limit");
        query.before_ms = page.back().started_ms;
        query.before_name = page.back().name;
        auto next = c.list(query);
        require(next.size() == 2 && page.back().name != next.front().name, "Stable page cursor");
        c.opened("history-a.mkv", "history", "Zulu");
        c.opened("history-z.mkv", "history", "Alpha");
        auto cameras = c.cameras();
        auto historical = std::find_if(cameras.begin(), cameras.end(),
                                       [](auto &entry) { return entry.first == "history"; });
        require(historical != cameras.end() && historical->second == "Alpha",
                "Historical camera name is not the latest");
        query.from_ms = INT64_MAX;
        require(c.list(query).empty(), "Date filter");

    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        std::filesystem::remove_all(root);
        return 1;
    }
    try {
        auto legacy = root / "legacy";
        std::filesystem::create_directories(legacy);
        sqlite3 *db{};
        require(sqlite3_open((legacy / "catalog.sqlite").c_str(), &db) == SQLITE_OK,
                "Legacy database");
        require(sqlite3_exec(
                    db,
                    "CREATE TABLE recordings(name TEXT PRIMARY KEY,channel TEXT NOT NULL,created "
                    "INTEGER NOT NULL,active INTEGER NOT NULL); INSERT INTO recordings "
                    "VALUES('old.mkv','old-camera',100,0),('crash.mkv','old-camera',101,1);",
                    nullptr, nullptr, nullptr) == SQLITE_OK,
                "Legacy schema");
        sqlite3_close(db);
        {
            Catalog c(legacy);
            auto rows = c.list();
            require(rows.size() == 2 && rows[0].state == "interrupted" && rows[1].state == "legacy",
                    "Legacy migration states");
        }
        {
            Catalog c(legacy);
            require(c.list().size() == 2, "Idempotent migration");
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
    std::cout << "Quota, active protection, ownership, symlinks and root lock passed\n";
}
