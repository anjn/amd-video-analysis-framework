#include "amdva/streaming/catalog.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace amdva::streaming;
namespace fs = std::filesystem;
void check(bool v, const char *message) {
    if (!v)
        throw std::runtime_error(message);
}
int main() {
    auto root = fs::temp_directory_path() / ("amdva-timeline-catalog-" + std::to_string(getpid()));
    try {
        Catalog c(root);
        auto add = [&](std::string name, int64_t start, int64_t length, std::string camera = "a") {
            c.opened(name, camera, "Camera", name, 0);
            std::ofstream(root / name) << "fixture";
            c.media_start(name, start);
            c.closed(name, length * 1000000);
        };
        add("a.mkv", 1000, 1000);
        add("b.mkv", 2000, 1000);
        add("c.mkv", 5000, 1000);
        add("other.mkv", 0, 999999, "b");
        auto coverage = c.coverage("a");
        check(coverage.oldest_ms == 1000 && coverage.intervals.size() == 2 &&
                  coverage.intervals[0].to_ms == 3000,
              "Coverage union and camera isolation");
        check(c.resolve("a", 1500)->name == "a.mkv", "Contains time");
        check(c.resolve("a", 2000)->name == "b.mkv", "Half-open boundary");
        check(c.resolve("a", 3000)->name == "c.mkv", "Gap resolves forward");
        check(!c.resolve("a", 6000), "After last recording");
        check(c.next(*c.resolve("a", 2000))->name == "c.mkv", "Next crosses sessions/gaps");
        add("same-z.mkv", 5000, 1000);
        check(c.next(*c.resolve("a", 5000))->name == "same-z.mkv", "Equal time stable ordering");
        fs::remove(root / "c.mkv");
        check(c.resolve("a", 3000)->name == "same-z.mkv", "Missing files skipped");
        add("unknown.mkv", 10000, -1); // make an explicit unknown duration below
        c.closed("unknown.mkv", -1);
        c.opened("active.mkv", "a");
        std::ofstream(root / "active.mkv") << "active";
        c.media_start("active.mkv", 11000);
        c.opened("broken.mkv", "a");
        c.interrupted("broken.mkv");
        check(c.coverage("a").pending_durations == 1, "Unknown duration indexing; active excluded");
        RecordingQuery q;
        q.channel = "a";
        q.from_ms = 1500;
        q.to_ms = 1500;
        auto rows = c.list(q);
        check(rows.size() == 1 && rows[0].started_ms == 1000,
              "Effective media timestamps in legacy list");
        auto pin = c.open_recording("b.mkv");
        check(pin->recording().started_ms == 2000, "Handle timestamp");
        // More than the former 10,000-fragment GUI limit. Adjacent files collapse
        // into one coverage interval, without truncating the oldest history.
        for (int i = 0; i < 10005; i++)
            add("long-" + std::to_string(i) + ".mkv", 20000 + i * 1000, 1000, "long");
        auto long_history = c.coverage("long");
        check(long_history.oldest_ms == 20000 && long_history.intervals.size() == 1 &&
                  long_history.intervals[0].to_ms == 10025000,
              "Untruncated long history");
        check(c.resolve("long", 20001)->name == "long-0.mkv", "Earliest file remains reachable");
        pin.reset();
        fs::remove_all(root);
        std::cout << "Wall-time queries, coverage, gaps, lifecycle and 10,005 fragments passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
