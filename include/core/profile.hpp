/// SPDX-License-Identifier: MIT

#pragma once

#include "core/raster.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace fy::core {

enum class BufferKind { framebuffer, ownership, scene, bins, statistics, worker_tiles, encoder, count };

struct BufferUsage {
    std::uint64_t current_bytes = 0;
    std::uint64_t peak_bytes = 0;
};

struct IoMetrics {
    std::uint64_t logical_bytes = 0;
    std::uint64_t decoded_bytes = 0;
    std::uint64_t read_calls = 0;
    std::uint64_t read_samples = 0;
};

struct PhaseMetrics {
    std::uint64_t calls = 0;
    double wall_seconds = 0;
};

struct CacheMetrics {
    std::uint64_t read_bytes = 0,write_bytes = 0,read_calls = 0,write_calls = 0,scenes = 0;
    std::uint64_t current_bytes = 0,peak_bytes = 0;
};

struct ProfileSnapshot {
    std::string scope;
    int requested_threads = 0;
    int thread_limit = 0;
    double wall_seconds = 0;
    std::optional<double> cpu_seconds;
    std::optional<std::uint64_t> process_peak_rss_bytes;
    std::optional<bool> success;
    std::optional<IoMetrics> io;
    std::optional<CacheMetrics> cache;
    std::optional<std::array<double,2>> shared_display_scale;
    std::optional<std::uint64_t> computed_samples;
    std::optional<std::uint64_t> source_pixels;
    std::optional<Shape> output;
    std::optional<std::uint64_t> scratch_bytes;
    std::optional<std::array<BufferUsage, static_cast<std::size_t>(BufferKind::count)>> buffers;
    std::uint64_t tracked_buffer_peak_bytes = 0;
    std::map<std::string, PhaseMetrics> phases;
};

class ProfileReport {
    struct State;
public:
    class Phase {
    public:
        Phase(Phase&&) noexcept;
        ~Phase();
        Phase(const Phase&) = delete;
        Phase& operator=(const Phase&) = delete;
    private:
        friend class ProfileReport;
        Phase(std::shared_ptr<State> state, std::string name);
        std::shared_ptr<State> state_;
        std::string name_;
        std::chrono::steady_clock::time_point begin_;
    };

    class BufferReservation {
    public:
        BufferReservation(BufferReservation&&) noexcept;
        BufferReservation& operator=(BufferReservation&&) noexcept;
        ~BufferReservation();
        void resize(std::uint64_t bytes);
        BufferReservation(const BufferReservation&) = delete;
        BufferReservation& operator=(const BufferReservation&) = delete;
    private:
        friend class ProfileReport;
        BufferReservation(std::shared_ptr<State> state, BufferKind kind, std::uint64_t bytes);
        std::shared_ptr<State> state_;
        BufferKind kind_;
        std::uint64_t bytes_;
    };

    ProfileReport(std::string scope, int requested_threads, int thread_limit);
    Phase phase(std::string name);
    BufferReservation reserve(BufferKind kind, std::uint64_t bytes);
    void record_io(IoMetrics delta) const;
    void record_cache_read(std::uint64_t bytes) const;
    void record_cache_write(std::uint64_t bytes) const;
    void release_cache(std::uint64_t bytes) const;
    void record_cached_scene();
    void set_shared_display_scale(std::array<double,2> scale);
    void record_computed(std::uint64_t samples);
    void set_scope(std::string scope);
    void set_source_pixels(std::uint64_t pixels);
    void set_output(Shape output);
    void set_scratch_bytes(std::uint64_t bytes);
    void finish(bool success);
    ProfileSnapshot snapshot() const;
    void write_json(const std::filesystem::path& path) const;

private:
    std::shared_ptr<State> state_;
};

}
