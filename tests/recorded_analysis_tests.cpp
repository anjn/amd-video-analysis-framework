#include "amdva/streaming/catalog.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace amdva::streaming;
void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
int main() {
    const auto root =
        std::filesystem::temp_directory_path() / ("amdva-analysis-" + std::to_string(getpid()));
    try {
        RecordedAnalysis frame{"cam", "session", "run",
                               1500,  1000,      {{{.1f, .2f, .3f, .4f, .9f, 0}, 7, 11}}};
        {
            Catalog c(root);
            std::ofstream(root / "one.mkv") << "fixture";
            c.opened("one.mkv", "cam", "Camera", "session", 0, 1000);
            const auto before_bytes = c.bytes();
            c.store_analysis({frame});
            check(c.bytes() > before_bytes, "Metadata contributes to storage quota");
            auto empty = frame;
            empty.time_ms = 2000;
            empty.objects.clear();
            c.store_analysis({empty});
            auto rows = c.analysis("cam", 1000, 2500);
            check(rows.size() == 2 && rows[0].objects.size() == 1 && rows[1].objects.empty(),
                  "Empty analysis clears detections");
            check(rows[0].objects[0].person == 7 && rows[0].objects[0].track == 11 &&
                      rows[0].objects[0].box.x == .1f,
                  "Persist normalized geometry and IDs");
            check(c.analysis("other", 0, 3000).empty(), "Camera isolation");
            check(c.analysis("cam", 1600, 1999).empty(), "Time range isolation");
            c.closed("one.mkv", 2 * GST_SECOND);
        }
        {
            Catalog c(root);
            check(c.analysis("cam", 0, 3000).size() == 2, "Restart preserves analysis");
            auto pin = c.open_recording("one.mkv");
            auto job = c.begin_delete();
            c.delete_batch(job);
            check(c.analysis("cam", 0, 3000).size() == 2, "Pinned recording preserves analysis");
            pin.reset();
            job = c.begin_delete();
            c.delete_batch(job);
            check(c.analysis("cam", 0, 3000).empty(), "Deletion removes analysis and objects");
            c.store_analysis({frame});
            check(c.analysis("cam", 0, 3000).empty(),
                  "Late completion cannot resurrect deleted data");
            std::ofstream(root / "two.mkv") << "fixture";
            c.opened("two.mkv", "cam", "Camera", "new-session");
            c.media_start("two.mkv", 1000);
            c.store_analysis({frame});
            check(c.analysis("cam", 0, 3000).empty(), "Wrong session rejected");
            frame.session = "new-session";
            c.enqueue_analysis(frame, std::make_shared<AiRuntime>());
            c.closed("two.mkv", 2 * GST_SECOND);
        }
        {
            Catalog c(root);
            check(c.analysis("cam", 0, 3000).size() == 1, "Writer drains at shutdown");
        }
        std::filesystem::remove(root / "two.mkv");
        {
            Catalog c(root);
            check(c.analysis("cam", 0, 3000).empty(), "Missing recording hides analysis");
        }
        std::filesystem::remove_all(root);
        std::cout << "Recorded analysis persistence, session/time isolation, empty frames, writer "
                     "drain and deletion passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
