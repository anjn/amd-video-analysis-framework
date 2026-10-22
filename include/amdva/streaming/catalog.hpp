#pragma once
#include "amdva/streaming/search.hpp"
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>
struct sqlite3;
struct sqlite3_stmt;
namespace amdva::streaming {
struct Recording {
    std::string name, channel, camera_name, session, state;
    int64_t started_ms{}, duration_ns{-1};
    unsigned fragment{};
    uint64_t size{};
    bool available{};
};
struct RecordingQuery {
    std::string channel;
    int64_t from_ms{}, to_ms{INT64_MAX}, before_ms{INT64_MAX};
    std::string before_name;
    unsigned limit{50};
};
struct RecordingInterval {
    int64_t from_ms{}, to_ms{}; // Half-open [from, to), receiver-side wall time.
};
struct RecordingCoverage {
    std::vector<RecordingInterval> intervals;
    std::optional<int64_t> oldest_ms;
    uint64_t pending_durations{};
};
struct StorageStatus {
    uint64_t recorded_bytes{}, available_bytes{};
};
struct DeletionJob {
    std::deque<std::string> pending;
    uint64_t total{}, processed{}, deleted{}, bytes{}, protected_files{}, failed{};
    bool done() const { return pending.empty(); }
};
class RecordingFile {
  public:
    ~RecordingFile();
    int fd() const { return fd_; }
    const Recording &recording() const { return recording_; }

  private:
    friend class Catalog;
    int fd_{-1};
    Recording recording_;
};
class Catalog {
  public:
    explicit Catalog(const std::filesystem::path &root);
    ~Catalog();
    Catalog(const Catalog &) = delete;
    Catalog &operator=(const Catalog &) = delete;
    void initialize_search();
    bool store_observation(const SearchObservation &);
    std::vector<SearchObservation> search(const std::array<float, 768> &, const std::string &model);
    std::vector<uint8_t> observation_image(int64_t id) const;
    void reconcile_search();
    void opened(const std::string &filename, const std::string &channel,
                const std::string &camera_name = {}, const std::string &session = {},
                unsigned fragment = 0);
    void closed(const std::string &filename, int64_t duration_ns = -1);
    // Correct new recording timestamps from the recorder's media clock.
    void media_start(const std::string &filename, int64_t started_ms);
    RecordingCoverage coverage(const std::string &channel) const;
    // Containing recording, or the first recording after the requested time.
    std::optional<Recording> resolve(const std::string &channel, int64_t at_ms,
                                     const std::string &exclude = {}) const;
    std::optional<Recording> next(const Recording &) const;
    // Container metadata only; bounded, no decoding. Call off the GUI thread.
    void index_unknown(const std::string &channel);
    void interrupted(const std::string &filename);
    void duration(const std::string &filename, int64_t duration_ns);
    std::vector<Recording> list(const RecordingQuery & = {}) const;
    std::shared_ptr<RecordingFile> open_recording(const std::string &filename);
    StorageStatus storage() const;
    std::vector<std::pair<std::string, std::string>> cameras() const;
    bool enforce(uint64_t quota, uint64_t reserve);
    uint64_t bytes() const;
    // Snapshot non-writing files. An optional cutoff includes only known ends <= cutoff.
    DeletionJob begin_delete(std::optional<int64_t> cutoff_ms = {}) const;
    void delete_batch(DeletionJob &); // Up to 32 files; checks a 10ms budget between entries.
    uint64_t deletion_generation() const { return deletion_generation_.load(); }
    const std::filesystem::path &root() const { return root_; }

  private:
    std::filesystem::path root_;
    sqlite3 *db_{};
    int directory_{-1}, lock_{-1};
    mutable std::recursive_mutex mutex_;
    std::vector<std::weak_ptr<RecordingFile>> playing_;
    std::set<std::string> duration_attempted_;
    std::atomic<uint64_t> deletion_generation_{0};
    Recording row(sqlite3_stmt *) const;
    void sql(const char *statement);
};
} // namespace amdva::streaming
