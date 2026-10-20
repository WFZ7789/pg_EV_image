/// SPDX-License-Identifier: MIT

#include "core/execution.hpp"

#include <algorithm>
#include <atomic>
#include <random>

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>

namespace fy::core {

ScratchDirectory::ScratchDirectory(const std::filesystem::path& parent) {
    base_ = std::filesystem::absolute(parent.empty() ? std::filesystem::temp_directory_path() : parent);
    if (std::filesystem::exists(base_) && !std::filesystem::is_directory(base_))
        throw std::invalid_argument("Scratch parent is not a directory");
}

const std::filesystem::path& ScratchDirectory::path() const {
    if (!path_.empty()) return path_;
    std::filesystem::create_directories(base_);
    std::random_device random;
    static std::atomic<std::uint64_t> sequence{0};
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto candidate = base_ / ("pg-ev-" + std::to_string(random()) + "-" + std::to_string(sequence++));
        if (std::filesystem::create_directory(candidate)) {
            path_ = std::move(candidate);
            return path_;
        }
    }
    throw std::runtime_error("Cannot create task scratch directory");
}

ScratchDirectory::~ScratchDirectory() {
    if (path_.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

std::uint64_t ScratchDirectory::bytes() const {
    if (path_.empty()) return 0;
    std::uint64_t result = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path_))
        if (entry.is_regular_file() && !entry.is_symlink()) result += entry.file_size();
    return result;
}

struct ExecutionContext::Impl {
    int threads;
    std::optional<oneapi::tbb::global_control> control;
    oneapi::tbb::task_arena arena;
    ProfileReport report;
    ScratchDirectory scratch;
    std::shared_ptr<IoChannel> channel;

    static int resolve(int requested) {
        if (requested < 0) throw std::invalid_argument("Thread count must be nonnegative");
        return requested ? requested : std::max(1, oneapi::tbb::info::default_concurrency());
    }

    Impl(const ExecutionOptions& options, std::string scope)
        : threads(resolve(options.threads)),
          control(options.threads ? std::make_optional<oneapi::tbb::global_control>(
              oneapi::tbb::global_control::max_allowed_parallelism,threads) : std::nullopt),arena(threads),
          report(std::move(scope), options.threads, threads), scratch(options.scratch_directory),
          channel(IoChannel::shared()) {}
};

ExecutionContext::ExecutionContext(const ExecutionOptions& options, std::string scope)
    : impl_(std::make_unique<Impl>(options, std::move(scope))) {}
ExecutionContext::~ExecutionContext() = default;
int ExecutionContext::thread_limit() const { return impl_->threads; }
ProfileReport& ExecutionContext::profile() { return impl_->report; }
IoChannel& ExecutionContext::io() { return *impl_->channel; }
const ScratchDirectory& ExecutionContext::scratch() const { return impl_->scratch; }

void ExecutionContext::parallel_rows(std::size_t rows,
                                     const std::function<void(std::size_t, std::size_t)>& action,
                                     std::size_t block_rows) {
    if (!action || !block_rows) throw std::invalid_argument("Invalid row action or block size");
    if (!rows) return;
    const auto blocks = rows / block_rows + (rows % block_rows != 0);
    impl_->arena.execute([&] {
        oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<std::size_t>(0, blocks), [&](const auto& range) {
            for (auto block = range.begin(); block != range.end(); ++block) {
                const auto begin = block * block_rows;
                const auto end = begin + std::min(block_rows, rows - begin);
                action(begin, end);
            }
        });
    });
}

}
