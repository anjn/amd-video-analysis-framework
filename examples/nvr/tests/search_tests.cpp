#include "backend/mobileclip_batch.hpp"
#include "amdva/streaming/catalog.hpp"
#include "backend/search.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>
using namespace amdva::nvr;
void require(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
int main() {
    auto root =
        std::filesystem::temp_directory_path() / ("amdva-search-" + std::to_string(getpid()));
    try {
        for (unsigned capacity : {1u, 4u}) {
            for (unsigned count = 2; count <= capacity + 1; ++count) {
                const auto gpu = mobileclip_gpu_count(count, capacity, false);
                require(gpu > 0 && gpu <= capacity && count - gpu == 1,
                        "Combined batch covers each crop once with both devices");
            }
            require(mobileclip_gpu_count(1, capacity, false) == 1 &&
                        mobileclip_gpu_count(1, capacity, true) == 0,
                    "Single-image collections alternate devices");
            bool rejected = false;
            try {
                mobileclip_gpu_count(capacity + 2, capacity, false);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            require(rejected, "Combined batch remains bounded");
        }
        PersonTracker tracker;
        PersonDetection red{{10, 10, 40, 100, .9f, 0}, {}}, blue{{100, 10, 40, 100, .9f, 0}, {}};
        red.feature[0] = 1;
        blue.feature[1] = 1;
        auto first = tracker.update(1000, {red, blue});
        require(!first[0].confirmed && first[0].person != first[1].person,
                "Tentative unique people");
        red.box.x += 3;
        blue.box.x -= 3;
        auto second = tracker.update(1200, {blue, red});
        require(second[0].confirmed && second[1].person == first[0].person &&
                    second[0].person == first[1].person,
                "Reordered detections preserve people");
        red.box.score = .2f;
        auto low = tracker.update(1400, {red, blue});
        require(low[0].person == first[0].person, "Low confidence recovery");
        tracker.update(1800, {});
        red.box.score = .9f;
        auto returned = tracker.update(5000, {red});
        require(returned[0].person == first[0].person && returned[0].track != first[0].track,
                "Reappearance reconnects person with new tracklet");
        require(!tracker.update(4900, {red})[0].confirmed, "Old frames rejected");
        auto expired = tracker.update(40000, {red});
        require(expired[0].person != first[0].person, "Expired gallery does not reconnect");
        PersonTracker ambiguous;
        auto same = red;
        same.box.x = 200;
        ambiguous.update(1000, {red, same});
        ambiguous.update(1200, {red, same});
        auto uncertain = ambiguous.update(5000, {red});
        require(uncertain[0].person > 2 && !uncertain[0].confirmed,
                "Ambiguous reappearance remains separate");
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
        {
            SearchGroup g{"a", "A", 0, 30000, .9, {}};
            for (int i = 0; i < 4; ++i) {
                SearchObservation o;
                o.time_ms = i * 10000;
                o.similarity = .6 + i * .1;
                o.recording = i < 2 ? "old.mkv" : "new.mkv";
                g.images.push_back(o);
            }
            auto present = [](const std::string &name) { return name != "old.mkv"; };
            require(prune_search_group(g, present) && g.images.size() == 2 &&
                        g.from_ms == 20000 && g.to_ms == 30000 &&
                        std::abs(g.similarity - .9) < 1e-9,
                    "Pruning removes deleted recordings and refreshes the span");
            require(!prune_search_group(g, present), "Unrelated deletions keep results");
            require(prune_search_group(g, [](const std::string &) { return false; }) &&
                        g.images.empty(),
                    "A fully deleted group becomes empty");
        }
        {
            std::array<float, 768> person{}, glasses{};
            person[0] = .8f;
            person[1] = .6f;
            glasses[1] = 1;
            const auto q = compose_text_query(person, {glasses}, .5f);
            double norm = 0;
            for (float v : q)
                norm += double(v) * v;
            require(std::abs(norm - 1) < 1e-6 && q[1] < .6f * q[0] / .8f,
                    "Negatives are subtracted and the query stays normalized");
            require(compose_text_query(person, {}) == person, "No negatives keeps the query");
        }
        std::vector<SearchObservation> hits;
        for (int i = 0; i <= 7; ++i) {
            SearchObservation o;
            o.id = i;
            o.channel = "a";
            o.time_ms = i * 10000;
            o.similarity = .5 + i * .01;
            hits.push_back(o);
        }
        {
            auto persons = hits;
            for (size_t i = 0; i < persons.size(); ++i) {
                persons[i].person_id = i % 2 ? "person-b" : "person-a";
                persons[i].track_id = "track";
            }
            auto grouped = group_search_results(persons);
            require(grouped.size() == 2 && grouped[0].person_id != grouped[1].person_id,
                    "Person identity overrides nearby time grouping");
            for (const auto &group : grouped)
                for (const auto &hit : group.images)
                    require(hit.person_id == group.person_id, "No mixed identities in cards");
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
            SearchIndex index(c);
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
            o.person_id = "camera/session/person-1";
            o.track_id = "track-1";
            o.reid[0] = 1;
            require(index.store(o), "Active observation accepted");
            require(index.search(o.embedding, "test").empty(), "Active recording hidden");
            c.closed("one.mkv", 10000000000LL);
            auto r = index.search(o.embedding, "test");
            require(r.size() == 1 && std::abs(r[0].similarity - 1) < 1e-6, "Cosine retrieval");
            require(index.image(r[0].id) == o.png, "Image persistence");
            require(r[0].person_id == o.person_id && r[0].track_id == o.track_id,
                    "Identity persistence");
            require(index.search(o.embedding, "other").empty(), "Model isolation");
            for (const auto &model : {"mobileclip-gpu", "mobileclip-npu"}) {
                auto mobile = o;
                mobile.model = model;
                mobile.person_id = model;
                mobile.embedding.fill(0);
                mobile.embedding[0] = .6f;
                mobile.embedding[511] = .8f;
                require(index.store(mobile),
                        "512D observation stored in zero-extended envelope");
                auto found = index.search(mobile.embedding, model);
                require(found.size() == 1 && found[0].person_id == model &&
                            std::abs(found[0].similarity - 1) < 1e-6,
                        "MobileCLIP backend isolation and zero-extension cosine");
            }
            require(index.search(o.embedding, "test").size() == 1, "Embedding spaces never mix");
            auto pin = c.open_recording("one.mkv");
            auto job = c.begin_delete();
            c.delete_batch(job);
            require(index.search(o.embedding, "test").size() == 1,
                    "Playback pins protect observations");
            pin.reset();
            job = c.begin_delete();
            c.delete_batch(job);
            require(index.search(o.embedding, "test").empty(), "Deletion cascades");
            require(!index.store(o), "Late completion cannot resurrect deleted recording");
            auto zero = o.embedding;
            zero.fill(0);
            bool rejected = false;
            try {
                index.search(zero, "test");
            } catch (...) {
                rejected = true;
            }
            require(rejected, "Invalid embedding rejected");
        }
        {
            Catalog reopened(root);
            SearchIndex reopened_index(reopened);
            std::array<float, 768> v{};
            v[0] = 1;
            require(reopened_index.search(v, "test").empty(), "Reopen migration");
        }
        {
            auto legacy = root / "legacy";
            SearchObservation old;
            old.channel = "legacy";
            old.session = "session";
            old.model = "legacy-model";
            old.time_ms = 1000;
            old.embedding[0] = 1;
            old.png = {1, 2, 3};
            {
                Catalog c(legacy);
                SearchIndex index(c);
                c.opened("old.mkv", old.channel, "Old camera", old.session, 0);
                std::ofstream(legacy / "old.mkv") << "fixture";
                c.media_start("old.mkv", 1000);
                require(index.store(old), "Legacy row stored");
                c.closed("old.mkv", 1000000000);
            }
            sqlite3 *db = nullptr;
            require(sqlite3_open((legacy / "catalog.sqlite").c_str(), &db) == SQLITE_OK,
                    "Open migration fixture");
            int rc = sqlite3_exec(db,
                                  "DROP INDEX observations_person; ALTER TABLE observations DROP "
                                  "COLUMN person_id; ALTER TABLE observations DROP COLUMN "
                                  "track_id; ALTER TABLE observations DROP COLUMN reid;",
                                  nullptr, nullptr, nullptr);
            sqlite3_close(db);
            require(rc == SQLITE_OK, "Build pre-ReID schema fixture");
            Catalog migrated(legacy);
            SearchIndex migrated_index(migrated);
            auto rows = migrated_index.search(old.embedding, old.model);
            require(rows.size() == 1 && rows[0].person_id.empty() &&
                        migrated_index.image(rows[0].id) == old.png,
                    "Migration preserves legacy image retrieval");
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
