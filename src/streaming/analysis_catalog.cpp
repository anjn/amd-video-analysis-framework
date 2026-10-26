#include "amdva/streaming/catalog.hpp"
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <thread>
namespace amdva::streaming {
namespace {
struct Query {
    sqlite3_stmt *p{};
    Query(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Query() { sqlite3_finalize(p); }
    void text(int n, const std::string &s) {
        sqlite3_bind_text(p, n, s.c_str(), -1, SQLITE_TRANSIENT);
    }
    void integer(int n, int64_t v) { sqlite3_bind_int64(p, n, v); }
    void real(int n, double v) { sqlite3_bind_double(p, n, v); }
    void done() {
        if (sqlite3_step(p) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
    }
};
std::string text(sqlite3_stmt *p, int n) {
    auto s = sqlite3_column_text(p, n);
    return s ? reinterpret_cast<const char *>(s) : "";
}
// Keypoints are stored as "x,y,confidence;..." text so rows stay compact and readable.
std::string encode_keypoints(const std::vector<RecordedKeypoint> &points) {
    std::string out;
    char buffer[96];
    for (const auto &p : points) {
        std::snprintf(buffer, sizeof(buffer), "%s%.6g,%.6g,%.6g", out.empty() ? "" : ";", p.x, p.y,
                      p.confidence);
        out += buffer;
    }
    return out;
}
std::vector<RecordedKeypoint> decode_keypoints(const std::string &text) {
    std::vector<RecordedKeypoint> out;
    std::istringstream stream(text);
    std::string item;
    while (std::getline(stream, item, ';')) {
        RecordedKeypoint p;
        if (std::sscanf(item.c_str(), "%f,%f,%f", &p.x, &p.y, &p.confidence) == 3)
            out.push_back(p);
    }
    return out;
}
bool has_column(sqlite3 *db, const char *table, const char *column) {
    Query q(db, (std::string("PRAGMA table_info(") + table + ")").c_str());
    while (sqlite3_step(q.p) == SQLITE_ROW)
        if (text(q.p, 1) == column)
            return true;
    return false;
}
} // namespace
struct Catalog::AnalysisWriter {
    struct Item {
        RecordedAnalysis frame;
        std::chrono::steady_clock::time_point queued;
    };
    AnalysisHooks hooks;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    std::thread thread;
    bool stopping{};
    std::string error;
};
void Catalog::initialize_analysis() {
    sql("CREATE TABLE IF NOT EXISTS analysis_frames(id INTEGER PRIMARY KEY,channel TEXT NOT "
        "NULL,session TEXT NOT NULL,run TEXT NOT NULL,time_ms INTEGER NOT NULL,valid_ms INTEGER "
        "NOT NULL);"
        "CREATE INDEX IF NOT EXISTS analysis_time ON analysis_frames(channel,time_ms);"
        "CREATE TABLE IF NOT EXISTS analysis_objects(frame INTEGER NOT NULL,x REAL,y REAL,w REAL,h "
        "REAL,score REAL,class_id INTEGER,person INTEGER,track INTEGER);"
        "CREATE INDEX IF NOT EXISTS analysis_objects_frame ON analysis_objects(frame);"
        "CREATE TRIGGER IF NOT EXISTS analysis_frame_delete AFTER DELETE ON analysis_frames BEGIN "
        "DELETE FROM analysis_objects WHERE frame=OLD.id; END;"
        "CREATE TRIGGER IF NOT EXISTS analysis_recording_delete AFTER DELETE ON recordings BEGIN "
        "DELETE FROM analysis_frames WHERE channel=OLD.channel AND session=OLD.session AND "
        "time_ms>=COALESCE(OLD.media_started_ms,OLD.created) AND (OLD.duration_ns<0 OR "
        "time_ms<COALESCE(OLD.media_started_ms,OLD.created)+(OLD.duration_ns+999999)/1000000); END;"
        "DELETE FROM analysis_frames WHERE NOT EXISTS(SELECT 1 FROM recordings r WHERE "
        "r.channel=analysis_frames.channel AND r.session=analysis_frames.session AND "
        "analysis_frames.time_ms>=COALESCE(r.media_started_ms,r.created) AND (r.active=1 OR "
        "(r.duration_ns>=0 AND "
        "analysis_frames.time_ms<COALESCE(r.media_started_ms,r.created)+(r.duration_ns+999999)/"
        "1000000)));");
    // Databases written before object detail existed gain nullable columns in place.
    for (const auto &[table, column] :
         {std::pair<const char *, const char *>{"analysis_objects", "label"},
          {"analysis_objects", "keypoints"},
          {"analysis_objects", "attributes"},
          {"analysis_frames", "custom"}})
        if (!has_column(db_, table, column))
            sql((std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " TEXT;").c_str());
    {
        Query records(db_,
                      "SELECT name,channel,session,COALESCE(media_started_ms,created),duration_ns "
                      "FROM recordings WHERE active=0");
        while (sqlite3_step(records.p) == SQLITE_ROW) {
            if (std::filesystem::is_regular_file(root_ / text(records.p, 0)))
                continue;
            Query remove(db_, "DELETE FROM analysis_frames WHERE channel=?1 AND session=?2 AND "
                              "time_ms>=?3 AND (?4<0 OR time_ms<?3+(?4+999999)/1000000)");
            remove.text(1, text(records.p, 1));
            remove.text(2, text(records.p, 2));
            remove.integer(3, sqlite3_column_int64(records.p, 3));
            remove.integer(4, sqlite3_column_int64(records.p, 4));
            remove.done();
        }
    }
    analysis_writer_ = std::make_shared<AnalysisWriter>();
    auto w = analysis_writer_;
    w->thread = std::thread([this, w] {
        for (;;) {
            std::vector<AnalysisWriter::Item> items;
            AnalysisHooks hooks;
            {
                std::unique_lock<std::mutex> lock(w->mutex);
                w->cv.wait_for(lock, std::chrono::milliseconds(250),
                               [&] { return w->stopping || w->queue.size() >= 32; });
                if (w->stopping && w->queue.empty())
                    break;
                while (!w->queue.empty() && items.size() < 64) {
                    items.push_back(std::move(w->queue.front()));
                    w->queue.pop_front();
                }
                hooks = w->hooks;
                if (!items.empty() && hooks.on_queue)
                    hooks.on_queue(w->queue.size(), 0);
            }
            if (items.empty())
                continue;
            auto begin = std::chrono::steady_clock::now();
            try {
                std::vector<RecordedAnalysis> batch;
                for (auto &item : items)
                    batch.push_back(std::move(item.frame));
                store_analysis(batch);
                if (hooks.on_saved)
                    hooks.on_saved(
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                  begin)
                            .count(),
                        std::chrono::duration<double, std::milli>(begin - items.front().queued)
                            .count(),
                        batch.size());
            } catch (const std::exception &e) {
                std::lock_guard<std::mutex> lock(w->mutex);
                w->error = e.what();
                if (hooks.on_failed)
                    hooks.on_failed(items.size());
            }
        }
    });
}
void Catalog::stop_analysis() {
    if (!analysis_writer_)
        return;
    {
        std::lock_guard<std::mutex> lock(analysis_writer_->mutex);
        analysis_writer_->stopping = true;
    }
    analysis_writer_->cv.notify_all();
    if (analysis_writer_->thread.joinable())
        analysis_writer_->thread.join();
}
void Catalog::analysis_hooks(AnalysisHooks hooks) {
    if (!analysis_writer_)
        return;
    std::lock_guard<std::mutex> lock(analysis_writer_->mutex);
    analysis_writer_->hooks = std::move(hooks);
}
void Catalog::enqueue_analysis(RecordedAnalysis frame) {
    if (!analysis_writer_ || frame.session.empty() || !frame.time_ms)
        return;
    auto &w = *analysis_writer_;
    std::lock_guard<std::mutex> lock(w.mutex);
    if (w.stopping)
        return;
    unsigned dropped = 0;
    if (w.queue.size() >= 1024) {
        w.queue.pop_front();
        dropped = 1;
    }
    w.queue.push_back({std::move(frame), std::chrono::steady_clock::now()});
    if (w.hooks.on_queue)
        w.hooks.on_queue(w.queue.size(), dropped);
    w.cv.notify_one();
}
std::string Catalog::analysis_error() const {
    if (!analysis_writer_)
        return {};
    std::lock_guard<std::mutex> lock(analysis_writer_->mutex);
    return analysis_writer_->error;
}
void Catalog::store_analysis(const std::vector<RecordedAnalysis> &frames) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    sql("BEGIN IMMEDIATE;");
    try {
        for (const auto &f : frames) {
            // Reject late queued results for recordings already deleted.
            Query q(db_,
                    "INSERT INTO analysis_frames(channel,session,run,time_ms,valid_ms,custom) SELECT "
                    "?1,?2,?3,?4,?5,?6 WHERE EXISTS(SELECT 1 FROM recordings r WHERE r.channel=?1 AND "
                    "r.session=?2 AND ?4>=COALESCE(r.media_started_ms,r.created) AND (r.active=1 "
                    "OR (r.duration_ns>=0 AND "
                    "?4<COALESCE(r.media_started_ms,r.created)+(r.duration_ns+999999)/1000000)))");
            q.text(1, f.channel);
            q.text(2, f.session);
            q.text(3, f.run);
            q.integer(4, f.time_ms);
            q.integer(5, std::min(2000u, f.valid_ms));
            if (f.custom_json.empty())
                sqlite3_bind_null(q.p, 6);
            else
                q.text(6, f.custom_json);
            q.done();
            if (!sqlite3_changes(db_))
                continue;
            auto id = sqlite3_last_insert_rowid(db_);
            for (const auto &o : f.objects) {
                Query b(db_, "INSERT INTO analysis_objects(frame,x,y,w,h,score,class_id,person,track,"
                             "label,keypoints,attributes) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
                b.integer(1, id);
                b.real(2, o.box.x);
                b.real(3, o.box.y);
                b.real(4, o.box.w);
                b.real(5, o.box.h);
                b.real(6, o.box.score);
                b.integer(7, o.box.class_id);
                b.integer(8, o.person);
                b.integer(9, o.track);
                // Empty detail is stored as NULL so plain detections stay as small as before.
                const auto points = encode_keypoints(o.keypoints);
                for (const auto &[index, value] :
                     {std::pair<int, const std::string *>{10, &o.label},
                      {11, &points},
                      {12, &o.attributes_json}})
                    if (value->empty())
                        sqlite3_bind_null(b.p, index);
                    else
                        b.text(index, *value);
                b.done();
            }
        }
        sql("COMMIT;");
    } catch (...) {
        sql("ROLLBACK;");
        throw;
    }
}
std::vector<RecordedAnalysis> Catalog::analysis(const std::string &channel, int64_t from,
                                                int64_t to) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Query q(
        db_,
        "SELECT f.id,f.channel,f.session,f.run,f.time_ms,f.valid_ms,f.custom FROM analysis_frames f WHERE "
        "f.channel=?1 AND f.time_ms>=?2 AND f.time_ms<=?3 AND EXISTS(SELECT 1 FROM recordings r "
        "WHERE r.channel=f.channel AND r.session=f.session AND "
        "f.time_ms>=COALESCE(r.media_started_ms,r.created) AND (r.active=1 OR (r.duration_ns>=0 "
        "AND f.time_ms<COALESCE(r.media_started_ms,r.created)+(r.duration_ns+999999)/1000000))) "
        "ORDER BY f.time_ms,f.id");
    q.text(1, channel);
    q.integer(2, from);
    q.integer(3, to);
    std::vector<RecordedAnalysis> out;
    while (sqlite3_step(q.p) == SQLITE_ROW) {
        RecordedAnalysis f{text(q.p, 1),
                           text(q.p, 2),
                           text(q.p, 3),
                           sqlite3_column_int64(q.p, 4),
                           unsigned(sqlite3_column_int(q.p, 5)),
                           {},
                           text(q.p, 6)};
        Query b(db_,
                "SELECT x,y,w,h,score,class_id,person,track,label,keypoints,attributes FROM "
                "analysis_objects WHERE frame=?1");
        b.integer(1, sqlite3_column_int64(q.p, 0));
        while (sqlite3_step(b.p) == SQLITE_ROW) {
            RecordedObject o;
            o.box.x = sqlite3_column_double(b.p, 0);
            o.box.y = sqlite3_column_double(b.p, 1);
            o.box.w = sqlite3_column_double(b.p, 2);
            o.box.h = sqlite3_column_double(b.p, 3);
            o.box.score = sqlite3_column_double(b.p, 4);
            o.box.class_id = sqlite3_column_int(b.p, 5);
            o.person = sqlite3_column_int64(b.p, 6);
            o.track = sqlite3_column_int64(b.p, 7);
            o.label = text(b.p, 8);
            o.keypoints = decode_keypoints(text(b.p, 9));
            o.attributes_json = text(b.p, 10);
            f.objects.push_back(o);
        }
        out.push_back(std::move(f));
    }
    return out;
}
} // namespace amdva::streaming
