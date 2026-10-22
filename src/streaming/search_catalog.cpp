#include "../../third_party/sqlite-vec/sqlite-vec.h"
#include "amdva/streaming/catalog.hpp"
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdexcept>
#include <sys/stat.h>
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
void Catalog::initialize_search() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (sqlite3_vec_init(db_, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("sqlite-vec initialization failed");
    sql("CREATE TABLE IF NOT EXISTS observations("
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
    reconcile_search();
}
void Catalog::reconcile_search() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    {
        Query missing(db_, "SELECT DISTINCT recording FROM observations WHERE recording<>''");
        std::vector<std::string> names;
        while (sqlite3_step(missing.p) == SQLITE_ROW) {
            auto name = str(missing.p, 0);
            struct stat st {};
            if (fstatat(directory_, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(st.st_mode))
                names.push_back(name);
        }
        for (const auto &name : names) {
            Query remove(db_, "DELETE FROM observations WHERE recording=?1");
            remove.text(1, name);
            remove.done();
        }
    }
    sql("UPDATE observations SET recording=COALESCE((SELECT name FROM recordings r WHERE "
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
bool Catalog::store_observation(const SearchObservation &o) {
    validate(o.embedding);
    if (o.png.empty() || o.png.size() > 4 * 1024 * 1024 || o.model.empty() || o.session.empty())
        throw std::invalid_argument("Invalid observation");
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // A deleted/retired recording cannot be resurrected by an in-flight inference.
    Query q(
        db_,
        "INSERT INTO observations(channel,session,time_ms,model,x,y,w,h,score,png,embedding) "
        "SELECT ?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11 WHERE EXISTS(SELECT 1 FROM recordings r WHERE "
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
    q.done();
    return sqlite3_changes(db_) != 0;
}
std::vector<SearchObservation> Catalog::search(const std::array<float, 768> &v,
                                               const std::string &model) {
    validate(v);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    reconcile_search();
    Query q(db_, "SELECT "
                 "o.id,o.channel,r.camera_name,o.time_ms,o.recording,1-vec_distance_cosine(o."
                 "embedding,?1) AS similarity "
                 "FROM observations o JOIN recordings r ON r.name=o.recording WHERE o.model=?2 AND "
                 "r.state='finalized' "
                 "ORDER BY similarity DESC,o.id LIMIT 500");
    q.blob(1, v.data(), sizeof(v));
    q.text(2, model);
    std::vector<SearchObservation> out;
    int rc;
    while ((rc = sqlite3_step(q.p)) == SQLITE_ROW) {
        SearchObservation o;
        o.id = sqlite3_column_int64(q.p, 0);
        o.channel = str(q.p, 1);
        o.camera_name = str(q.p, 2);
        o.time_ms = sqlite3_column_int64(q.p, 3);
        o.recording = str(q.p, 4);
        o.similarity = sqlite3_column_double(q.p, 5);
        // Match the catalog's no-symlink/missing-file checks without pinning every hit.
        struct stat st {};
        if (fstatat(directory_, o.recording.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(st.st_mode))
            continue;
        out.push_back(std::move(o));
    }
    if (rc != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
    return out;
}
std::vector<uint8_t> Catalog::observation_image(int64_t id) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Query q(db_, "SELECT png FROM observations WHERE id=?1");
    sqlite3_bind_int64(q.p, 1, id);
    if (sqlite3_step(q.p) != SQLITE_ROW)
        return {};
    auto *p = static_cast<const uint8_t *>(sqlite3_column_blob(q.p, 0));
    return {p, p + sqlite3_column_bytes(q.p, 0)};
}
} // namespace amdva::streaming
