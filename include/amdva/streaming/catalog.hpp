#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
struct sqlite3;
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
struct StorageStatus {
    uint64_t recorded_bytes{}, available_bytes{};
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
    void opened(const std::string &filename, const std::string &channel,
                const std::string &camera_name = {}, const std::string &session = {},
                unsigned fragment = 0);
    void closed(const std::string &filename, int64_t duration_ns = -1);
    void interrupted(const std::string &filename);
    void duration(const std::string &filename, int64_t duration_ns);
    std::vector<Recording> list(const RecordingQuery & = {}) const;
    std::shared_ptr<RecordingFile> open_recording(const std::string &filename);
    StorageStatus storage() const;
    std::vector<std::pair<std::string, std::string>> cameras() const;
    bool enforce(uint64_t quota, uint64_t reserve);
    uint64_t bytes() const;
    const std::filesystem::path &root() const { return root_; }

  private:
    std::filesystem::path root_;
    sqlite3 *db_{};
    int directory_{-1}, lock_{-1};
    mutable std::recursive_mutex mutex_;
    std::vector<std::weak_ptr<RecordingFile>> playing_;
    void sql(const char *statement);
};
} // namespace amdva::streaming
