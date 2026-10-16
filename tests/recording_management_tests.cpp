#include "amdva/streaming/catalog.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace amdva::streaming;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main() {
    auto root =
        fs::temp_directory_path() / ("amdva-recording-management-" + std::to_string(getpid()));
    try {
        Catalog catalog(root);
        auto add = [&](std::string name, int64_t start, int64_t duration,
                       std::string camera = "a") {
            catalog.opened(name, camera);
            std::ofstream(root / name) << "recording";
            catalog.media_start(name, start);
            catalog.closed(name, duration * 1000000);
        };
        add("old.mkv", 0, 1000);
        add("edge.mkv", 1000, 1000);
        add("overlap.mkv", 1500, 1000);
        add("pin.mkv", 0, 1000, "b");
        auto pin = catalog.open_recording("pin.mkv");
        catalog.opened("active.mkv", "a");
        std::ofstream(root / "active.mkv") << "active";
        add("unknown.mkv", 0, -1);
        catalog.closed("unknown.mkv", -1);
        std::ofstream(root / "external.mkv") << "external";
        auto job = catalog.begin_delete(2000);
        check(job.total == 3, "Expiration end boundary/unknown/active");
        while (!job.done())
            catalog.delete_batch(job);
        check(job.deleted == 2 && job.protected_files == 1 && job.failed == 0,
              "Expired removal and pin protection");
        check(fs::exists(root / "overlap.mkv") && fs::exists(root / "unknown.mkv"),
              "Overlap or unknown deleted");
        pin.reset();
        job = catalog.begin_delete(2000);
        while (!job.done())
            catalog.delete_batch(job);
        check(!fs::exists(root / "pin.mkv"), "Released expired file not removed");
        // Manual snapshot must not include files which were still writing at its start.
        job = catalog.begin_delete();
        catalog.closed("active.mkv", 1000000000);
        add("new.mkv", 4000, 1000);
        while (!job.done())
            catalog.delete_batch(job);
        check(fs::exists(root / "active.mkv") && fs::exists(root / "new.mkv") &&
                  fs::exists(root / "external.mkv"),
              "Manual snapshot included active/new/external files");
        catalog.opened("link.mkv", "a");
        fs::create_symlink(root / "external.mkv", root / "link.mkv");
        catalog.closed("link.mkv");
        catalog.opened("missing.mkv", "a");
        catalog.closed("missing.mkv");
        for (int i = 0; i < 70; i++)
            add("batch-" + std::to_string(i) + ".mkv", 5000 + i, 1);
        job = catalog.begin_delete();
        catalog.delete_batch(job);
        check(job.processed <= 32 && !job.done(), "Deletion batch is unbounded");
        while (!job.done())
            catalog.delete_batch(job);
        check(job.failed == 1 && fs::is_symlink(root / "link.mkv") &&
                  fs::exists(root / "external.mkv"),
              "Symlink/external protection");
        check(catalog.deletion_generation() > 0, "Deletion invalidation generation missing");
        fs::remove_all(root);
        std::cout
            << "Expiry, pins, manual snapshot, batch limits, missing files and symlinks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        fs::remove_all(root);
        return 1;
    }
}
