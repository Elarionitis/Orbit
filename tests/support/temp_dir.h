#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace orbit::testing {

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        m_path = std::filesystem::temp_directory_path() /
                 ("orbit-test-" + std::to_string(::getpid()) + "-" +
                  std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

}  // namespace orbit::testing
