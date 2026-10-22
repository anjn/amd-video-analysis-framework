#include "amdva/streaming/catalog.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace amdva::streaming;
void require(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
int main() {
    auto root =
        std::filesystem::temp_directory_path() / ("amdva-search-" + std::to_string(getpid()));
    try {
        auto dims = search_dimensions(100, 900, 384);
        require(dims.first == 43 && dims.second == 384, "Portrait resize");
        require(search_dimensions(20, 10, 384) == std::pair<unsigned, unsigned>(20, 10),
                "No upscale");
        SearchImage im{2, 1, {1, 2, 3, 4, 5, 6}};
        auto rgb = search_letterbox(im, 32);
        require(rgb.size() == 32 * 32 * 3 && rgb[(15 * 32 + 15) * 3] == 1 && rgb[0] == 0,
                "Centered padding");
        amdva::Detection b;
        b.x = -10;
        b.y = 5;
        b.w = 30;
        b.h = 30;
        b = search_crop(b, 100, 20, .1);
        require(b.x == 0 && b.y == 2 && b.w == 23 && b.h == 18, "Clipped margin");
        std::vector<SearchObservation> hits;
        for (int i = 0; i <= 7; ++i) {
            SearchObservation o;
            o.id = i;
            o.channel = "a";
            o.time_ms = i * 10000;
            o.similarity = .5 + i * .01;
            hits.push_back(o);
        }
        SearchObservation other;
        other.channel = "b";
        other.time_ms = 10000;
        other.similarity = .9;
        hits.push_back(other);
        auto groups = group_search_results(hits);
        require(groups.size() == 3 && groups[0].channel == "b", "Camera/time grouping");
        require(groups[1].images.size() == 1 && groups[2].images.size() == 7,
                "60-second cap and 10-second inclusive boundary");
        {
            Catalog c(root);
            std::ofstream(root / "one.mkv") << "fixture";
            c.opened("one.mkv", "a", "Camera", "s");
            c.media_start("one.mkv", 1000);
            SearchObservation o;
            o.channel = "a";
            o.session = "s";
            o.time_ms = 2000;
            o.model = "test";
            o.embedding[0] = 1;
            o.png = {1, 2, 3};
            require(c.store_observation(o), "Active observation accepted");
            require(c.search(o.embedding, "test").empty(), "Active recording hidden");
            c.closed("one.mkv", 10000000000LL);
            auto r = c.search(o.embedding, "test");
            require(r.size() == 1 && std::abs(r[0].similarity - 1) < 1e-6, "Cosine retrieval");
            require(c.observation_image(r[0].id) == o.png, "Image persistence");
            require(c.search(o.embedding, "other").empty(), "Model isolation");
            auto pin = c.open_recording("one.mkv");
            auto job = c.begin_delete();
            c.delete_batch(job);
            require(c.search(o.embedding, "test").size() == 1,
                    "Playback pins protect observations");
            pin.reset();
            job = c.begin_delete();
            c.delete_batch(job);
            require(c.search(o.embedding, "test").empty(), "Deletion cascades");
            require(!c.store_observation(o), "Late completion cannot resurrect deleted recording");
            auto zero = o.embedding;
            zero.fill(0);
            bool rejected = false;
            try {
                c.search(zero, "test");
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Invalid embedding rejected");
        }
        {
            Catalog reopened(root);
            std::array<float, 768> v{};
            v[0] = 1;
            require(reopened.search(v, "test").empty(), "Reopen migration");
        }
        std::filesystem::remove_all(root);
        std::cout << "search contract passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
