/// SPDX-License-Identifier: MIT

#pragma once

#include "core/adapter.hpp"
#include "core/profile.hpp"

namespace fy::core {

struct ExecutionOptions {
    int threads = 0;
    bool profile = false;
    std::filesystem::path scratch_directory;
};

class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::filesystem::path& parent = {});
    ~ScratchDirectory();
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    const std::filesystem::path& path() const;
    std::uint64_t bytes() const;
private:
    std::filesystem::path base_;
    mutable std::filesystem::path path_;
};

class ExecutionContext {
public:
    explicit ExecutionContext(const ExecutionOptions& options, std::string scope = "core");
    ~ExecutionContext();
    ExecutionContext(const ExecutionContext&) = delete;
    ExecutionContext& operator=(const ExecutionContext&) = delete;

    int thread_limit() const;
    ProfileReport& profile();
    IoChannel& io();
    const ScratchDirectory& scratch() const;
    void parallel_rows(std::size_t rows, const std::function<void(std::size_t, std::size_t)>& action,
                       std::size_t block_rows = 256);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
