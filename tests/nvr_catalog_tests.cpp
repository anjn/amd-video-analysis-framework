#include "catalog.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
using amdva::nvr::Catalog;
static void require(bool b, const char *message) {
    if (!b)
        throw std::runtime_error(message);
}
int main() {
    auto root =
        std::filesystem::temp_directory_path() / ("amdva-catalog-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        Catalog c(root);
        std::ofstream(root / "external.mkv") << std::string(100, 'x');
        c.opened("old.mkv", "a");
        std::ofstream(root / "old.mkv") << std::string(100, 'a');
        c.closed("old.mkv");
        c.opened("active.mkv", "b");
        std::ofstream(root / "active.mkv") << std::string(100, 'b');
        c.opened("link.mkv", "c");
        std::filesystem::create_symlink(root / "external.mkv", root / "link.mkv");
        c.closed("link.mkv");
        require(c.bytes() == 200, "Catalog byte accounting");
        require(c.enforce(100, 0), "Cannot prune closed recording");
        require(!std::filesystem::exists(root / "old.mkv"), "Old recording retained");
        require(std::filesystem::exists(root / "active.mkv"), "Active recording deleted");
        require(std::filesystem::exists(root / "external.mkv"), "External recording deleted");
        require(std::filesystem::is_symlink(root / "link.mkv"), "Symlink deleted");
        require(!c.enforce(1, 0), "Active-only quota should fail");
        bool rejected = false;
        try {
            c.opened("../outside.mkv", "x");
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Traversal accepted");
        rejected = false;
        try {
            Catalog second(root);
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Concurrent writer accepted");
        c.closed("active.mkv");
        require(c.enforce(1, 0), "Finalized recording not eligible");
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
    std::cout << "Quota, active protection, ownership, symlinks and root lock passed\n";
}
