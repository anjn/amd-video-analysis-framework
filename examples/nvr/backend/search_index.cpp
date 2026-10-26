#include "backend/search.hpp"
#include "amdva/streaming/catalog.hpp"
#include "sqlite-vec.h"
#include <cmath>
#include <cstring>
#include <sqlite3.h>
#include <stdexcept>
namespace amdva::nvr {
using namespace amdva::streaming;
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
    void blob(int n, const void *p, size_t size) {
        sqlite3_bind_blob(this->p, n, p, int(size), SQLITE_TRANSIENT);
    }
    void done() {
        if (sqlite3_step(p) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
    }
};
std::string str(sqlite3_stmt *p, int n) {
    auto *s = sqlite3_column_text(p, n);
    return s ? reinterpret_cast<const char *>(s) : "";
}
void exec(sqlite3 *db, const char *statement) {
    char *error{};
    if (sqlite3_exec(db, statement, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string text = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error(text);
    }
}
void validate(const std::array<float, 768> &v) {
    double norm = 0;
    for (auto f : v) {
        if (!std::isfinite(f))
            throw std::invalid_argument("Non-finite embedding");
        norm += f * f;
    }
    if (norm < .99 || norm > 1.01)
        throw std::invalid_argument("Embedding must be normalized");
}
} // namespace
SearchIndex::SearchIndex(Catalog &catalog) : catalog_(catalog) {
    catalog_.extend({[this](sqlite3 *db) { initialize(db); },
                     [](sqlite3 *db) -> uint64_t {
                         Query payload(db, "SELECT COALESCE(SUM(length(png)+length(embedding)+"
                                           "COALESCE(length(reid),0)+256),0) FROM observations");
                         return sqlite3_step(payload.p) == SQLITE_ROW
                                    ? uint64_t(sqlite3_column_int64(payload.p, 0))
                                    : 0;
                     }});
}
void SearchIndex::initialize(sqlite3 *db) {
    if (sqlite3_vec_init(db, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("sqlite-vec initialization failed");
    exec(db, "CREATE TABLE IF NOT EXISTS observations("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,channel TEXT NOT NULL,session TEXT NOT NULL,time_ms "
        "INTEGER NOT "
        "NULL,"
        "recording TEXT NOT NULL DEFAULT '',model TEXT NOT NULL,x REAL,y REAL,w REAL,h REAL,score "
        "REAL,"
        "png BLOB NOT NULL,embedding BLOB NOT NULL CHECK(length(embedding)=3072));"
        "CREATE INDEX IF NOT EXISTS observations_session_time ON observations(session,time_ms);"
        "CREATE INDEX IF NOT EXISTS observations_recording ON observations(recording);"
        "CREATE INDEX IF NOT EXISTS observations_model ON observations(model);"
        "CREATE TRIGGER IF NOT EXISTS observations_delete AFTER DELETE ON recordings BEGIN "
        "DELETE FROM observations WHERE recording=OLD.name OR "
        "(recording='' AND session=OLD.session AND "
        "time_ms>=COALESCE(OLD.media_started_ms,OLD.created) "
        "AND OLD.duration_ns>=0 AND "
        "time_ms<COALESCE(OLD.media_started_ms,OLD.created)+(OLD.duration_ns+999999)/1000000); "
        "END;");
    for (const auto *column : {"person_id", "track_id", "reid"}) {
        Query info(db, "PRAGMA table_info(observations)");
        bool found = false;
        while (sqlite3_step(info.p) == SQLITE_ROW)
            found |= str(info.p, 1) == column;
        if (!found) {
            std::string alter =
                std::string("ALTER TABLE observations ADD COLUMN ") + column +
                (std::string(column) == "reid" ? " BLOB" : " TEXT NOT NULL DEFAULT ''");
            exec(db, alter.c_str());
        }
    }
    exec(db, "CREATE INDEX IF NOT EXISTS observations_person ON "
             "observations(model,channel,person_id);");
    reconcile(db);
}
void SearchIndex::reconcile() {
    catalog_.with_database([this](sqlite3 *db) { reconcile(db); });
}
void SearchIndex::reconcile(sqlite3 *db) {
    {
        Query missing(db, "SELECT DISTINCT recording FROM observations WHERE recording<>''");
        std::vector<std::string> names;
        while (sqlite3_step(missing.p) == SQLITE_ROW) {
            auto name = str(missing.p, 0);
            if (!catalog_.recording_present(name))
                names.push_back(name);
        }
        for (const auto &name : names) {
            Query remove(db, "DELETE FROM observations WHERE recording=?1");
            remove.text(1, name);
            remove.done();
        }
    }
    exec(db, "UPDATE observations SET recording=COALESCE((SELECT name FROM recordings r WHERE "
        "r.channel=observations.channel AND r.session=observations.session AND r.state='finalized' "
        "AND "
        "observations.time_ms>=COALESCE(r.media_started_ms,r.created) AND "
        "observations.time_ms<COALESCE(r.media_started_ms,r.created)+(r.duration_ns+999999)/"
        "1000000 "
        "ORDER BY COALESCE(r.media_started_ms,r.created) DESC,name LIMIT 1),'') WHERE recording='';"
        "DELETE FROM observations WHERE (recording<>'' AND NOT EXISTS(SELECT 1 FROM recordings r "
        "WHERE r.name=recording AND r.state='finalized')) OR "
        "(recording='' AND NOT EXISTS(SELECT 1 FROM recordings r WHERE "
        "r.session=observations.session AND r.active=1 AND "
        "observations.time_ms>=COALESCE(r.media_started_ms,r.created)));");
}
bool SearchIndex::store(const SearchObservation &o) {
    validate(o.embedding);
    if (!o.person_id.empty()) {
        double norm = 0;
        for (float value : o.reid) {
            if (!std::isfinite(value))
                throw std::invalid_argument("Invalid ReID feature");
            norm += value * value;
        }
        if (o.track_id.empty() || norm < .99 || norm > 1.01)
            throw std::invalid_argument("Invalid person identity");
    }
    if (o.png.empty() || o.png.size() > 4 * 1024 * 1024 || o.model.empty() || o.session.empty())
        throw std::invalid_argument("Invalid observation");
    bool stored = false;
    catalog_.with_database([&](sqlite3 *db) {
        // A deleted/retired recording cannot be resurrected by an in-flight inference.
        Query q(db, "INSERT INTO "
                    "observations(channel,session,time_ms,model,x,y,w,h,score,png,embedding,"
                    "person_id,track_id,reid) "
                    "SELECT ?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14 WHERE EXISTS(SELECT 1 "
                    "FROM recordings r WHERE "
                    "r.channel=?1 AND r.session=?2 AND ((r.active=1 AND "
                    "?3>=COALESCE(r.media_started_ms,r.created)) OR (r.state='finalized' AND "
                    "?3>=COALESCE(r.media_started_ms,r.created) AND "
                    "?3<COALESCE(r.media_started_ms,r.created)+(r.duration_ns+999999)/1000000)))");
        q.text(1, o.channel);
        q.text(2, o.session);
        sqlite3_bind_int64(q.p, 3, o.time_ms);
        q.text(4, o.model);
        sqlite3_bind_double(q.p, 5, o.box.x);
        sqlite3_bind_double(q.p, 6, o.box.y);
        sqlite3_bind_double(q.p, 7, o.box.w);
        sqlite3_bind_double(q.p, 8, o.box.h);
        sqlite3_bind_double(q.p, 9, o.box.score);
        q.blob(10, o.png.data(), o.png.size());
        q.blob(11, o.embedding.data(), sizeof(o.embedding));
        q.text(12, o.person_id);
        q.text(13, o.track_id);
        if (!o.person_id.empty())
            q.blob(14, o.reid.data(), sizeof(o.reid));
        q.done();
        stored = sqlite3_changes(db) != 0;
    });
    return stored;
}
std::vector<SearchObservation> SearchIndex::search(const std::array<float, 768> &v,
                                                   const std::string &model) {
    validate(v);
    std::vector<SearchObservation> out;
    catalog_.with_database([&](sqlite3 *db) {
        reconcile(db);
        Query q(db, "SELECT "
                    "o.id,o.channel,r.camera_name,o.time_ms,o.recording,1-vec_distance_cosine(o."
                    "embedding,?1) AS similarity,o.person_id,o.track_id "
                    "FROM observations o JOIN recordings r ON r.name=o.recording WHERE "
                    "o.model=?2 AND r.state='finalized' "
                    "ORDER BY similarity DESC,o.id LIMIT 500");
        q.blob(1, v.data(), sizeof(v));
        q.text(2, model);
        int rc;
        while ((rc = sqlite3_step(q.p)) == SQLITE_ROW) {
            SearchObservation o;
            o.id = sqlite3_column_int64(q.p, 0);
            o.channel = str(q.p, 1);
            o.camera_name = str(q.p, 2);
            o.time_ms = sqlite3_column_int64(q.p, 3);
            o.recording = str(q.p, 4);
            o.similarity = sqlite3_column_double(q.p, 5);
            o.person_id = str(q.p, 6);
            o.track_id = str(q.p, 7);
            // Match the catalog's no-symlink/missing-file checks without pinning every hit.
            if (!catalog_.recording_present(o.recording))
                continue;
            out.push_back(std::move(o));
        }
        if (rc != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db));
    });
    return out;
}
std::vector<uint8_t> SearchIndex::image(int64_t id) const {
    std::vector<uint8_t> out;
    catalog_.with_database([&](sqlite3 *db) {
        Query q(db, "SELECT png FROM observations WHERE id=?1");
        sqlite3_bind_int64(q.p, 1, id);
        if (sqlite3_step(q.p) != SQLITE_ROW)
            return;
        auto *p = static_cast<const uint8_t *>(sqlite3_column_blob(q.p, 0));
        out.assign(p, p + sqlite3_column_bytes(q.p, 0));
    });
    return out;
}
} // namespace amdva::nvr
