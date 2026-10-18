/// SPDX-License-Identifier: MIT

#include "core/execution.hpp"

#include <atomic>
#include <fstream>
#include <thread>

namespace {

void require(bool condition) {
    if (!condition) throw std::runtime_error("Execution check failed");
}

template<class F>
void rejects(F&& action) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Expected execution failure");
}

}

int main() {
    using namespace fy::core;
    ScratchDirectory test_root;
    auto sentinel = test_root.path() / "keep.txt";
    std::ofstream(sentinel) << "keep";
    for (int threads : {1, 4, 0}) {
        std::filesystem::path scratch;
        {
            ExecutionContext context({threads, false, test_root.path()});
            require(context.thread_limit() >= 1);
            if (threads) require(context.thread_limit() == threads);
            scratch = context.scratch().path();
            require(std::filesystem::is_directory(scratch));
            std::ofstream(scratch / "cached.bin", std::ios::binary) << "1234567";
            require(context.scratch().bytes() == 7);
            constexpr std::size_t rows = 2099;
            std::vector<std::atomic<int>> visits(rows);
            std::atomic<int> active{0}, maximum{0};
            context.parallel_rows(rows, [&](std::size_t begin, std::size_t end) {
                require(begin % 256 == 0 && end <= rows && end > begin && end - begin <= 256);
                auto current = ++active;
                auto previous = maximum.load();
                while (previous < current && !maximum.compare_exchange_weak(previous, current)) {}
                for (auto row = begin; row < end; ++row) ++visits[row];
                --active;
            });
            for (const auto& visited : visits) require(visited == 1);
            require(maximum <= context.thread_limit());
            require(context.io().run([] { return std::this_thread::get_id(); }) != std::this_thread::get_id());
            bool called = false;
            context.parallel_rows(0, [&](auto, auto) { called = true; });
            require(!called);
            rejects([&] { context.parallel_rows(rows, [](auto, auto) {}, 0); });
            rejects([&] { context.parallel_rows(rows, [](auto, auto) { throw std::runtime_error("Kernel error"); }); });
            context.parallel_rows(1, [&](auto begin, auto end) { require(begin == 0 && end == 1); });
        }
        require(!std::filesystem::exists(scratch) && std::filesystem::exists(sentinel));
    }
    rejects([&] { ExecutionContext context({-1, false, test_root.path()}); });
    rejects([&] { ScratchDirectory scratch(sentinel); });
    require(std::filesystem::exists(sentinel));

    ProfileReport report("test\n\"escaped\"", 4, 4);
    require(!report.snapshot().io && !report.snapshot().buffers);
    {
        auto outer = report.phase("outer");
        auto a = report.reserve(BufferKind::scene, 100);
        {
            auto inner = report.phase("inner");
            auto b = report.reserve(BufferKind::worker_tiles, 60);
            auto moved = std::move(b);
            require(report.snapshot().tracked_buffer_peak_bytes == 160);
        }
        require((*report.snapshot().buffers)[static_cast<std::size_t>(BufferKind::worker_tiles)].current_bytes == 0);
    }
    {
        auto a = report.reserve(BufferKind::bins, 120);
        a.resize(150);
        a.resize(90);
        auto b = report.reserve(BufferKind::scene, 40);
        b = std::move(a);
        auto usage = report.snapshot();
        require((*usage.buffers)[static_cast<std::size_t>(BufferKind::bins)].current_bytes == 90);
        require((*usage.buffers)[static_cast<std::size_t>(BufferKind::scene)].current_bytes == 0);
        rejects([&] { a.resize(1); });
        auto c = report.reserve(BufferKind::scene, 1);
        rejects([&] { b.resize(std::numeric_limits<std::uint64_t>::max()); });
        require((*report.snapshot().buffers)[static_cast<std::size_t>(BufferKind::bins)].current_bytes == 90);
    }
    report.record_io({100, 200, 1, 50});
    report.record_io({20, 40, 2, 10});
    report.record_computed(60);
    report.set_source_pixels(50);
    report.set_output({20, 30});
    report.set_scratch_bytes(7);
    report.finish(true);
    auto snapshot = report.snapshot();
    require(snapshot.success == true && snapshot.wall_seconds >= 0);
    require(snapshot.io->logical_bytes == 120 && snapshot.io->decoded_bytes == 240 && snapshot.io->read_calls == 3);
    require(snapshot.computed_samples == 60 && snapshot.output == Shape{20, 30});
    require(snapshot.tracked_buffer_peak_bytes == 160);
    for (auto usage : *snapshot.buffers) require(usage.current_bytes == 0);
    require(snapshot.phases.at("outer").calls == 1 && snapshot.phases.at("inner").calls == 1);
    rejects([&] { report.finish(true); });
    auto path = test_root.path() / "profile.json";
    report.write_json(path);
    std::ifstream file(path);
    std::string text{std::istreambuf_iterator<char>(file), {}};
    require(text.find("test\\u000a\\\"escaped\\\"") != std::string::npos);
    require(text.find("\"tracked_buffer_peak_bytes\": 160") != std::string::npos);
    require(text.find("\"phase_times_may_overlap\": true") != std::string::npos);
}
