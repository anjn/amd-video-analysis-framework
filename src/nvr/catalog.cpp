#include "catalog.hpp"
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace amdva::nvr {
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
        sql("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; CREATE TABLE IF NOT EXISTS "
            "recordings(name TEXT PRIMARY KEY, channel TEXT NOT NULL, created INTEGER NOT NULL, "
            "active INTEGER NOT NULL);");
        // Exclusive root lock proves no previous writer is alive. Crash leftovers
        // stay catalogued, but are not advertised as successfully finalized.
        sql("UPDATE recordings SET active=0 WHERE active=1;");
    } catch (...) {
        if (db_)
            sqlite3_close(db_);
        close(lock_);
        close(directory_);
        throw;
    }
}
Catalog::~Catalog() {
    if (db_)
        sqlite3_close(db_);
    if (lock_ >= 0)
        close(lock_);
    if (directory_ >= 0)
        close(directory_);
}
void Catalog::opened(const std::string &name, const std::string &channel) {
    if (!safe(name))
        throw std::runtime_error("Unsafe recording filename");
    Statement s(db_, "INSERT INTO recordings VALUES(?1,?2,?3,1)");
    s.text(1, name);
    s.text(2, channel);
    sqlite3_bind_int64(s.p, 3,
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count());
    s.done();
}
void Catalog::closed(const std::string &name) {
    Statement s(db_, "UPDATE recordings SET active=0 WHERE name=?1");
    s.text(1, name);
    s.done();
}
uint64_t Catalog::bytes() const {
    Statement s(db_, "SELECT name FROM recordings");
    uint64_t total = 0;
    while (sqlite3_step(s.p) == SQLITE_ROW) {
        const std::string name = reinterpret_cast<const char *>(sqlite3_column_text(s.p, 0));
        struct stat st {};
        if (safe(name) && fstatat(directory_, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(st.st_mode))
            total += uint64_t(st.st_size);
    }
    return total;
}
bool Catalog::enforce(uint64_t quota, uint64_t reserve) {
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
        // unlinkat never follows a replacement symlink. The candidate came only
        // from this application's catalog; no directory scanning is used.
        if (unlinkat(directory_, item.first.c_str(), 0) != 0)
            continue;
        Statement s(db_, "DELETE FROM recordings WHERE name=?1 AND active=0");
        s.text(1, item.first);
        s.done();
        total = total > item.second ? total - item.second : 0;
        free = std::filesystem::space(root_).available;
        if (total <= quota && free >= reserve)
            return true;
    }
    return false;
}
} // namespace amdva::nvr
