#include "amdva/streaming/catalog.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <gst/gst.h>
#include <sqlite3.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace amdva::streaming {
namespace {
struct Statement {
    sqlite3_stmt *p{};
    Statement(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(p); }
    void text(int n, const std::string &v) {
        sqlite3_bind_text(p, n, v.c_str(), -1, SQLITE_TRANSIENT);
    }
    void done() {
        if (sqlite3_step(p) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p)));
    }
};
bool safe(const std::string &name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.size() > 4 && name.substr(name.size() - 4) == ".mkv";
}
} // namespace
void Catalog::sql(const char *statement) {
    if (sqlite3_exec(db_, statement, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db_));
}
Catalog::Catalog(const std::filesystem::path &root) {
    std::filesystem::create_directories(root);
    root_ = std::filesystem::canonical(root);
    directory_ = open(root_.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_ < 0)
        throw std::runtime_error("Cannot open recording root");
    lock_ = openat(directory_, ".nvr.lock", O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0 || flock(lock_, LOCK_EX | LOCK_NB) < 0) {
        if (lock_ >= 0)
            close(lock_);
        close(directory_);
        throw std::runtime_error("Recording root already in use");
    }
    try {
        if (sqlite3_open_v2((root_ / "catalog.sqlite").c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
                                SQLITE_OPEN_NOFOLLOW,
                            nullptr) != SQLITE_OK)
            throw std::runtime_error("Cannot open recording catalog");
        {
            Statement mode(db_, "PRAGMA auto_vacuum");
            if (sqlite3_step(mode.p) == SQLITE_ROW && sqlite3_column_int(mode.p, 0) == 0) {
                // Migrate once so removing crop BLOBs can return disk space.
                sqlite3_finalize(mode.p);
                mode.p = nullptr;
                sql("PRAGMA auto_vacuum=INCREMENTAL; VACUUM;");
            }
        }
        sql("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS "
            "recordings(name TEXT PRIMARY KEY, channel TEXT NOT NULL, created INTEGER NOT NULL, "
            "active INTEGER NOT NULL);");
        // Exclusive root lock proves no previous writer is alive. Crash leftovers
        // stay catalogued, but are not advertised as successfully finalized.
        // Idempotent migration of the original four-column catalog.
        sql("BEGIN IMMEDIATE;");
        try {
            std::vector<std::string> columns;
            {
                Statement q(db_, "PRAGMA table_info(recordings)");
                while (sqlite3_step(q.p) == SQLITE_ROW)
                    columns.emplace_back(
                        reinterpret_cast<const char *>(sqlite3_column_text(q.p, 1)));
            }
            for (const auto &column : std::vector<std::pair<std::string, std::string>>{
                     {"camera_name", "TEXT NOT NULL DEFAULT ''"},
                     {"session", "TEXT NOT NULL DEFAULT ''"},
                     {"fragment", "INTEGER NOT NULL DEFAULT 0"},
                     {"duration_ns", "INTEGER NOT NULL DEFAULT -1"},
                     {"state", "TEXT NOT NULL DEFAULT 'legacy'"},
                     {"media_started_ms", "INTEGER"}})
                if (std::find(columns.begin(), columns.end(), column.first) == columns.end())
                    sql(("ALTER TABLE recordings ADD COLUMN " + column.first + " " + column.second)
                            .c_str());
            sql("UPDATE recordings SET state='interrupted',active=0 WHERE active=1;");
            sql("CREATE INDEX IF NOT EXISTS recordings_time ON recordings(created DESC,name "
                "DESC);");
            sql("CREATE INDEX IF NOT EXISTS recordings_channel_time ON recordings(channel,created "
                "DESC,name DESC);");
            sql("CREATE INDEX IF NOT EXISTS recordings_media_time ON recordings"
                "(channel,COALESCE(media_started_ms,created),name);");
            sql("COMMIT;");
            initialize_analysis();
        } catch (...) {
            sql("ROLLBACK;");
            throw;
        }
    } catch (...) {
        if (db_)
            sqlite3_close(db_);
        close(lock_);
        close(directory_);
        throw;
    }
}
Catalog::~Catalog() {
    stop_analysis();
    if (db_)
        sqlite3_close(db_);
    if (lock_ >= 0)
        close(lock_);
    if (directory_ >= 0)
        close(directory_);
}
void Catalog::opened(const std::string &name, const std::string &channel,
                     const std::string &camera_name, const std::string &session, unsigned fragment,
                     std::optional<int64_t> media_started_ms) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!safe(name))
        throw std::runtime_error("Unsafe recording filename");
    Statement s(db_, "INSERT INTO "
                     "recordings(name,channel,created,active,camera_name,session,fragment,state,"
                     "media_started_ms) "
                     "VALUES(?1,?2,?3,1,?4,?5,?6,'recording',?7)");
    s.text(4, camera_name);
    s.text(5, session);
    sqlite3_bind_int64(s.p, 6, fragment);
    if (media_started_ms)
        sqlite3_bind_int64(s.p, 7, *media_started_ms);
    else
        sqlite3_bind_null(s.p, 7);
    s.text(1, name);
    s.text(2, channel);
    sqlite3_bind_int64(s.p, 3,
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count());
    s.done();
}
void Catalog::closed(const std::string &name, int64_t duration_ns) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement s(db_,
                "UPDATE recordings SET active=0,state='finalized',duration_ns=?2 WHERE name=?1");
    sqlite3_bind_int64(s.p, 2, duration_ns);
    s.text(1, name);
    s.done();
}
void Catalog::extend(CatalogExtension extension) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (extension.initialize)
        extension.initialize(db_);
    extensions_.push_back(std::move(extension));
}
void Catalog::with_database(const std::function<void(sqlite3 *)> &f) const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    f(db_);
}
bool Catalog::recording_present(const std::string &name) const {
    struct stat st {};
    return safe(name) && fstatat(directory_, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
           S_ISREG(st.st_mode);
}
uint64_t Catalog::bytes() const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement s(db_, "SELECT name FROM recordings");
    uint64_t total = 0;
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        const std::string name = reinterpret_cast<const char *>(sqlite3_column_text(s.p, 0));
        struct stat st {};
        if (safe(name) && fstatat(directory_, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(st.st_mode))
            total += uint64_t(st.st_size);
    }
    for (const auto &extension : extensions_)
        if (extension.bytes)
            total += extension.bytes(db_);
    Statement analysis_payload(
        db_, "SELECT COALESCE((SELECT SUM(length(channel)+length(session)+length(run)+192) FROM "
             "analysis_frames),0) + (SELECT COUNT(*)*96 FROM analysis_objects)");
    if (sqlite3_step(analysis_payload.p) == SQLITE_ROW)
        total += sqlite3_column_int64(analysis_payload.p, 0);
    return total;
}
bool Catalog::enforce(uint64_t quota, uint64_t reserve) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    playing_.erase(
        std::remove_if(playing_.begin(), playing_.end(), [](auto &p) { return p.expired(); }),
        playing_.end());
    uint64_t total = bytes();
    auto free = std::filesystem::space(root_).available;
    if (total <= quota && free >= reserve)
        return true;
    std::vector<std::pair<std::string, uint64_t>> candidates;
    {
        Statement s(db_, "SELECT name FROM recordings WHERE active=0 ORDER BY created,name");
        while (sqlite3_step(s.p) == SQLITE_ROW) {
            const std::string name = reinterpret_cast<const char *>(sqlite3_column_text(s.p, 0));
            struct stat st {};
            if (safe(name) && fstatat(directory_, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISREG(st.st_mode))
                candidates.emplace_back(name, uint64_t(st.st_size));
        }
    }
    for (const auto &item : candidates) {
        bool pinned = false;
        for (const auto &weak : playing_)
            if (auto file = weak.lock())
                pinned |= file->recording().name == item.first;
        if (pinned)
            continue;
        // unlinkat never follows a replacement symlink. The candidate came only
        // from this application's catalog; no directory scanning is used.
        if (unlinkat(directory_, item.first.c_str(), 0) != 0)
            continue;
        Statement s(db_, "DELETE FROM recordings WHERE name=?1 AND active=0");
        s.text(1, item.first);
        s.done();
        ++deletion_generation_;
        sql("PRAGMA incremental_vacuum(256);");
        total = bytes();
        free = std::filesystem::space(root_).available;
        if (total <= quota && free >= reserve)
            return true;
    }
    return false;
}

DeletionJob Catalog::begin_delete(std::optional<int64_t> cutoff) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Statement q(db_, "SELECT name FROM recordings WHERE active=0 AND "
                     "(?1=0 OR (duration_ns>=0 AND COALESCE(media_started_ms,created)+"
                     "(duration_ns+999999)/1000000<=?2)) ORDER BY created,name");
    sqlite3_bind_int(q.p, 1, cutoff.has_value());
    sqlite3_bind_int64(q.p, 2, cutoff.value_or(0));
    DeletionJob job;
    int result;
    while ((result = sqlite3_step(q.p)) == SQLITE_ROW)
        job.pending.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(q.p, 0)));
    if (result != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(db_));
    job.total = job.pending.size();
    return job;
}
void Catalog::delete_batch(DeletionJob &job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    playing_.erase(
        std::remove_if(playing_.begin(), playing_.end(), [](auto &p) { return p.expired(); }),
        playing_.end());
    unsigned count = 0;
    while (!job.pending.empty() && count++ < 32 && std::chrono::steady_clock::now() < deadline) {
        auto name = std::move(job.pending.front());
        job.pending.pop_front();
        job.processed++;
        bool pinned = false;
        for (auto &weak : playing_)
            if (auto file = weak.lock())
                pinned |= file->recording().name == name;
        if (pinned) {
            job.protected_files++;
            continue;
        }
        try {
            Statement q(db_, "SELECT active FROM recordings WHERE name=?1");
            q.text(1, name);
            const auto result = sqlite3_step(q.p);
            if (result == SQLITE_DONE)
                continue;
            if (result != SQLITE_ROW)
                throw std::runtime_error(sqlite3_errmsg(db_));
            if (sqlite3_column_int(q.p, 0)) {
                job.protected_files++;
                continue;
            }
            struct stat st {};
            bool exists = fstatat(directory_, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
            if (!safe(name) || (exists && !S_ISREG(st.st_mode)) || (!exists && errno != ENOENT)) {
                job.failed++;
                continue;
            }
            if (exists && unlinkat(directory_, name.c_str(), 0) != 0 && errno != ENOENT) {
                job.failed++;
                continue;
            }
            ++deletion_generation_;
            Statement remove(db_, "DELETE FROM recordings WHERE name=?1 AND active=0");
            remove.text(1, name);
            remove.done();
            duration_attempted_.erase(name);
            if (exists) {
                job.deleted++;
                job.bytes += uint64_t(st.st_size);
            }
        } catch (...) {
            job.failed++;
        }
    }
    sql("PRAGMA incremental_vacuum(256);");
}

RecordingFile::~RecordingFile() {
    if (fd_ >= 0)
        ::close(fd_);
}
void Catalog::media_start(const std::string &name, int64_t ms) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement s(db_, "UPDATE recordings SET media_started_ms=?2 WHERE name=?1");
    s.text(1, name);
    sqlite3_bind_int64(s.p, 2, ms);
    s.done();
}
Recording Catalog::row(sqlite3_stmt *p) const {
    auto text = [&](int n) {
        return std::string(reinterpret_cast<const char *>(sqlite3_column_text(p, n)));
    };
    Recording r;
    r.name = text(0);
    r.channel = text(1);
    r.camera_name = text(2);
    r.session = text(3);
    r.state = text(4);
    r.started_ms = sqlite3_column_int64(p, 5);
    r.duration_ns = sqlite3_column_int64(p, 6);
    r.fragment = sqlite3_column_int(p, 7);
    struct stat st {};
    r.available = safe(r.name) &&
                  fstatat(directory_, r.name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                  S_ISREG(st.st_mode);
    if (r.available)
        r.size = st.st_size;
    return r;
}
RecordingCoverage Catalog::coverage(const std::string &channel) const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement q(db_,
                "SELECT "
                "name,channel,camera_name,session,state,COALESCE(media_started_ms,created),"
                "duration_ns,fragment "
                "FROM recordings WHERE channel=?1 AND active=0 AND state IN ('finalized','legacy') "
                "ORDER BY COALESCE(media_started_ms,created),name");
    q.text(1, channel);
    RecordingCoverage out;
    while (sqlite3_step(q.p) == SQLITE_ROW) {
        auto r = row(q.p);
        if (!r.available)
            continue;
        if (!out.oldest_ms)
            out.oldest_ms = r.started_ms;
        if (r.duration_ns <= 0) {
            if (r.duration_ns < 0)
                out.pending_durations++;
            continue;
        }
        const auto end = r.started_ms + (r.duration_ns + 999999) / 1000000;
        if (!out.intervals.empty() && out.intervals.back().to_ms >= r.started_ms)
            out.intervals.back().to_ms = std::max(out.intervals.back().to_ms, end);
        else
            out.intervals.push_back({r.started_ms, end});
    }
    return out;
}
std::optional<Recording> Catalog::resolve(const std::string &channel, int64_t at,
                                          const std::string &exclude) const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement q(db_,
                "SELECT "
                "name,channel,camera_name,session,state,COALESCE(media_started_ms,created),"
                "duration_ns,fragment "
                "FROM recordings WHERE channel=?1 AND active=0 AND state IN ('finalized','legacy') "
                "AND duration_ns>0 AND name!=?3 AND "
                "(COALESCE(media_started_ms,created)>=?2 OR "
                "COALESCE(media_started_ms,created)+(duration_ns+999999)/1000000>?2) "
                "ORDER BY (COALESCE(media_started_ms,created)<=?2) DESC, "
                "CASE WHEN COALESCE(media_started_ms,created)<=?2 THEN "
                "COALESCE(media_started_ms,created) END DESC, "
                "COALESCE(media_started_ms,created),name");
    q.text(1, channel);
    sqlite3_bind_int64(q.p, 2, at);
    q.text(3, exclude);
    while (sqlite3_step(q.p) == SQLITE_ROW) {
        auto r = row(q.p);
        if (r.available)
            return r;
    }
    return {};
}
std::optional<Recording> Catalog::next(const Recording &r) const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement q(db_, "SELECT "
                     "name,channel,camera_name,session,state,COALESCE(media_started_ms,created),"
                     "duration_ns,fragment "
                     "FROM recordings WHERE channel=?1 AND active=0 AND state IN "
                     "('finalized','legacy') AND duration_ns>0 "
                     "AND (COALESCE(media_started_ms,created)>?2 OR "
                     "(COALESCE(media_started_ms,created)=?2 AND name>?3)) "
                     "ORDER BY COALESCE(media_started_ms,created),name");
    q.text(1, r.channel);
    sqlite3_bind_int64(q.p, 2, r.started_ms);
    q.text(3, r.name);
    while (sqlite3_step(q.p) == SQLITE_ROW) {
        auto candidate = row(q.p);
        if (candidate.available)
            return candidate;
    }
    return {};
}
void Catalog::index_unknown(const std::string &channel) {
    std::shared_ptr<RecordingFile> file;
    {
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        Statement q(db_,
                    "SELECT name FROM recordings WHERE channel=?1 AND active=0 AND duration_ns<0 "
                    "AND state IN ('finalized','legacy') ORDER BY created,name");
        q.text(1, channel);
        while (sqlite3_step(q.p) == SQLITE_ROW) {
            std::string name(reinterpret_cast<const char *>(sqlite3_column_text(q.p, 0)));
            if (!duration_attempted_.insert(name).second)
                continue;
            try {
                file = open_recording(name);
            } catch (...) {
                continue;
            }
            break;
        }
    }
    if (!file)
        return;
    GError *error{};
    const auto description =
        "fdsrc fd=" + std::to_string(file->fd()) + " ! matroskademux ! fakesink sync=false";
    auto *pipeline = gst_parse_launch(description.c_str(), &error);
    if (error || !pipeline) {
        g_clear_error(&error);
        if (pipeline)
            gst_object_unref(pipeline);
        return;
    }
    if (gst_element_set_state(pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE) {
        gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
        gint64 ns = -1;
        if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &ns) && ns > 0)
            duration(file->recording().name, ns);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}
void Catalog::interrupted(const std::string &name) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement s(db_,
                "UPDATE recordings SET active=0,state='interrupted' WHERE name=?1 AND active=1");
    s.text(1, name);
    s.done();
}
std::vector<Recording> Catalog::list(const RecordingQuery &q) const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement s(db_,
                "SELECT "
                "name,channel,camera_name,session,state,COALESCE(media_started_ms,created),"
                "duration_ns,fragment FROM "
                "recordings WHERE (?1='' OR channel=?1) AND "
                "(COALESCE(media_started_ms,created)>=?2 OR (duration_ns>=0 AND "
                "COALESCE(media_started_ms,created)+duration_ns/1000000>=?2)) AND "
                "COALESCE(media_started_ms,created)<=?3 AND (COALESCE(media_started_ms,created)<?4 "
                "OR (COALESCE(media_started_ms,created)=?4 "
                "AND (?5='' OR name<?5))) ORDER BY COALESCE(media_started_ms,created) DESC,name "
                "DESC LIMIT ?6");
    s.text(1, q.channel);
    sqlite3_bind_int64(s.p, 2, q.from_ms);
    sqlite3_bind_int64(s.p, 3, q.to_ms);
    sqlite3_bind_int64(s.p, 4, q.before_ms);
    s.text(5, q.before_name);
    sqlite3_bind_int(s.p, 6, std::clamp(q.limit, 1u, 200u));
    std::vector<Recording> result;
    int status;
    while ((status = sqlite3_step(s.p)) == SQLITE_ROW) {
        result.push_back(row(s.p));
    }
    if (status != SQLITE_DONE)
        throw std::runtime_error("Cannot query recordings");
    return result;
}
std::shared_ptr<RecordingFile> Catalog::open_recording(const std::string &name) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!safe(name))
        throw std::runtime_error("Unsafe recording filename");
    Statement q(db_, "SELECT "
                     "active,state,channel,camera_name,session,COALESCE(media_started_ms,created),"
                     "duration_ns,fragment "
                     "FROM recordings WHERE name=?1");
    q.text(1, name);
    if (sqlite3_step(q.p) != SQLITE_ROW || sqlite3_column_int(q.p, 0))
        throw std::runtime_error("Recording is unavailable or still active");
    auto text = [&](int n) {
        return std::string(reinterpret_cast<const char *>(sqlite3_column_text(q.p, n)));
    };
    if (text(1) != "finalized" && text(1) != "legacy")
        throw std::runtime_error("Recording was interrupted");
    auto result = std::make_shared<RecordingFile>();
    result->fd_ = openat(directory_, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st {};
    if (result->fd_ < 0 || fstat(result->fd_, &st) != 0 || !S_ISREG(st.st_mode))
        throw std::runtime_error("Recording file is unavailable");
    auto &r = result->recording_;
    r.name = name;
    r.state = text(1);
    r.channel = text(2);
    r.camera_name = text(3);
    r.session = text(4);
    r.started_ms = sqlite3_column_int64(q.p, 5);
    r.duration_ns = sqlite3_column_int64(q.p, 6);
    r.fragment = sqlite3_column_int(q.p, 7);
    r.size = st.st_size;
    r.available = true;
    playing_.push_back(result);
    return result;
}
StorageStatus Catalog::storage() const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return {bytes(), std::filesystem::space(root_).available};
}
std::vector<std::pair<std::string, std::string>> Catalog::cameras() const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement s(db_, "SELECT groups.channel,(SELECT COALESCE(NULLIF(camera_name,''),channel) "
                     "FROM recordings newest WHERE newest.channel=groups.channel "
                     "ORDER BY created DESC,name DESC LIMIT 1) FROM "
                     "(SELECT DISTINCT channel FROM recordings) groups ORDER BY groups.channel");
    std::vector<std::pair<std::string, std::string>> result;
    while (sqlite3_step(s.p) == SQLITE_ROW)
        result.emplace_back(reinterpret_cast<const char *>(sqlite3_column_text(s.p, 0)),
                            reinterpret_cast<const char *>(sqlite3_column_text(s.p, 1)));
    return result;
}
void Catalog::duration(const std::string &name, int64_t ns) {
    if (ns < 0)
        return;
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    Statement s(db_, "UPDATE recordings SET duration_ns=?2 WHERE name=?1 AND active=0");
    s.text(1, name);
    sqlite3_bind_int64(s.p, 2, ns);
    s.done();
}
} // namespace amdva::streaming
