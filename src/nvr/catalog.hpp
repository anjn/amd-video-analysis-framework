#pragma once
#include <cstdint>
#include <filesystem>
#include <sqlite3.h>
#include <string>
namespace amdva::nvr {
class Catalog {
  public:
    explicit Catalog(const std::filesystem::path &root);
    ~Catalog();
    Catalog(const Catalog &) = delete;
    Catalog &operator=(const Catalog &) = delete;
    void opened(const std::string &filename, const std::string &channel);
    void closed(const std::string &filename);
    bool enforce(uint64_t quota, uint64_t reserve);
    uint64_t bytes() const;
    const std::filesystem::path &root() const { return root_; }

  private:
    std::filesystem::path root_;
    sqlite3 *db_{};
    int directory_{-1}, lock_{-1};
    void sql(const char *statement);
};
} // namespace amdva::nvr
