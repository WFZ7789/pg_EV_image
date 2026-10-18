/// SPDX-License-Identifier: MIT

#include "core/profile.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <locale>
#include <limits>
#include <mutex>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace fy::core {
namespace {

using Clock = std::chrono::steady_clock;

std::optional<std::uint64_t> peak_rss() {
#if defined(__APPLE__) || defined(__linux__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) return {};
    auto result = static_cast<std::uint64_t>(usage.ru_maxrss);
#if defined(__linux__)
    result *= 1024;
#endif
    return result;
#else
    return {};
#endif
}

void quoted(std::ostream& output, const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') output << '\\' << static_cast<char>(c);
        else if (c < 32) output << "\\u00" << hex[c >> 4] << hex[c & 15];
        else output << static_cast<char>(c);
    }
    output << '"';
}

template<class T>
void nullable(std::ostream& output, const std::optional<T>& value) {
    if (value) output << *value;
    else output << "null";
}

}

struct ProfileReport::State {
    mutable std::mutex mutex;
    ProfileSnapshot data;
    Clock::time_point begin = Clock::now();
    std::clock_t cpu_begin = std::clock();
    std::uint64_t current_buffer_bytes = 0;
    bool finished = false;

    void measure_time() {
        data.wall_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        const auto cpu_end = std::clock();
        if (cpu_begin != std::clock_t(-1) && cpu_end != std::clock_t(-1) && cpu_end >= cpu_begin)
            data.cpu_seconds = static_cast<double>(cpu_end - cpu_begin) / CLOCKS_PER_SEC;
        data.process_peak_rss_bytes = peak_rss();
    }
};

ProfileReport::Phase::Phase(std::shared_ptr<State> state, std::string name)
    : state_(std::move(state)), name_(std::move(name)), begin_(Clock::now()) {
    std::lock_guard lock(state_->mutex);
    state_->data.phases.try_emplace(name_);
}

ProfileReport::Phase::Phase(Phase&&) noexcept = default;

ProfileReport::Phase::~Phase() {
    if (!state_) return;
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin_).count();
    std::lock_guard lock(state_->mutex);
    auto& phase = state_->data.phases.at(name_);
    ++phase.calls;
    phase.wall_seconds += elapsed;
}

ProfileReport::BufferReservation::BufferReservation(std::shared_ptr<State> state, BufferKind kind,
                                                   std::uint64_t bytes)
    : state_(std::move(state)), kind_(kind), bytes_(bytes) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= static_cast<std::size_t>(BufferKind::count)) throw std::invalid_argument("Invalid buffer kind");
    std::lock_guard lock(state_->mutex);
    if (bytes > std::numeric_limits<std::uint64_t>::max() - state_->current_buffer_bytes)
        throw std::overflow_error("Buffer accounting overflow");
    if (!state_->data.buffers) state_->data.buffers.emplace();
    auto& usage = (*state_->data.buffers)[index];
    usage.current_bytes += bytes;
    usage.peak_bytes = std::max(usage.peak_bytes, usage.current_bytes);
    state_->current_buffer_bytes += bytes;
    state_->data.tracked_buffer_peak_bytes = std::max(state_->data.tracked_buffer_peak_bytes,
                                                     state_->current_buffer_bytes);
}

ProfileReport::BufferReservation::BufferReservation(BufferReservation&&) noexcept = default;

ProfileReport::BufferReservation& ProfileReport::BufferReservation::operator=(BufferReservation&& other) noexcept {
    if (this != &other) {
        if (state_) resize(0);
        state_ = std::move(other.state_);
        kind_ = other.kind_;
        bytes_ = other.bytes_;
    }
    return *this;
}

void ProfileReport::BufferReservation::resize(std::uint64_t bytes) {
    if (!state_) throw std::logic_error("Moved buffer reservation");
    std::lock_guard lock(state_->mutex);
    if (bytes > bytes_ && bytes - bytes_ > std::numeric_limits<std::uint64_t>::max() - state_->current_buffer_bytes)
        throw std::overflow_error("Buffer accounting overflow");
    auto& usage = (*state_->data.buffers)[static_cast<std::size_t>(kind_)];
    usage.current_bytes = usage.current_bytes - bytes_ + bytes;
    state_->current_buffer_bytes = state_->current_buffer_bytes - bytes_ + bytes;
    bytes_ = bytes;
    usage.peak_bytes = std::max(usage.peak_bytes,usage.current_bytes);
    state_->data.tracked_buffer_peak_bytes = std::max(state_->data.tracked_buffer_peak_bytes,state_->current_buffer_bytes);
}

ProfileReport::BufferReservation::~BufferReservation() {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    (*state_->data.buffers)[static_cast<std::size_t>(kind_)].current_bytes -= bytes_;
    state_->current_buffer_bytes -= bytes_;
}

ProfileReport::ProfileReport(std::string scope, int requested_threads, int thread_limit)
    : state_(std::make_shared<State>()) {
    state_->data.scope = std::move(scope);
    state_->data.requested_threads = requested_threads;
    state_->data.thread_limit = thread_limit;
}

ProfileReport::Phase ProfileReport::phase(std::string name) { return {state_, std::move(name)}; }

ProfileReport::BufferReservation ProfileReport::reserve(BufferKind kind, std::uint64_t bytes) {
    return {state_, kind, bytes};
}

void ProfileReport::record_io(IoMetrics delta) const {
    std::lock_guard lock(state_->mutex);
    if (!state_->data.io) state_->data.io.emplace();
    auto& io = *state_->data.io;
    io.logical_bytes += delta.logical_bytes;
    io.decoded_bytes += delta.decoded_bytes;
    io.read_calls += delta.read_calls;
    io.read_samples += delta.read_samples;
}

void ProfileReport::record_computed(std::uint64_t samples) {
    std::lock_guard lock(state_->mutex);
    state_->data.computed_samples = state_->data.computed_samples.value_or(0) + samples;
}

void ProfileReport::record_cache_read(std::uint64_t bytes) const {
    std::lock_guard lock(state_->mutex);
    if (!state_->data.cache) state_->data.cache.emplace();
    state_->data.cache->read_bytes += bytes; ++state_->data.cache->read_calls;
}

void ProfileReport::record_cache_write(std::uint64_t bytes) const {
    std::lock_guard lock(state_->mutex);
    if (!state_->data.cache) state_->data.cache.emplace();
    auto& cache = *state_->data.cache;
    cache.write_bytes += bytes; ++cache.write_calls;
    cache.current_bytes += bytes; cache.peak_bytes = std::max(cache.peak_bytes,cache.current_bytes);
}

void ProfileReport::release_cache(std::uint64_t bytes) const {
    std::lock_guard lock(state_->mutex);
    if (!state_->data.cache || bytes > state_->data.cache->current_bytes) throw std::logic_error("Cache accounting underflow");
    state_->data.cache->current_bytes -= bytes;
}

void ProfileReport::record_cached_scene() {
    std::lock_guard lock(state_->mutex);
    if (!state_->data.cache) state_->data.cache.emplace();
    ++state_->data.cache->scenes;
}

void ProfileReport::set_shared_display_scale(std::array<double,2> scale) {
    if (!std::isfinite(scale[0]) || !std::isfinite(scale[1])) throw std::invalid_argument("Nonfinite shared display scale");
    std::lock_guard lock(state_->mutex);
    state_->data.shared_display_scale = scale;
}

void ProfileReport::set_source_pixels(std::uint64_t pixels) {
    std::lock_guard lock(state_->mutex);
    state_->data.source_pixels = pixels;
}

void ProfileReport::set_scope(std::string scope) {
    if (scope.empty()) throw std::invalid_argument("Empty profile scope");
    std::lock_guard lock(state_->mutex);
    if (state_->finished) throw std::logic_error("Profile already finished");
    state_->data.scope = std::move(scope);
}

void ProfileReport::set_output(Shape output) {
    if (!output.elements()) throw std::invalid_argument("Empty output dimensions");
    std::lock_guard lock(state_->mutex);
    state_->data.output = output;
}

void ProfileReport::set_scratch_bytes(std::uint64_t bytes) {
    std::lock_guard lock(state_->mutex);
    state_->data.scratch_bytes = bytes;
}

void ProfileReport::finish(bool success) {
    std::lock_guard lock(state_->mutex);
    if (state_->finished) throw std::logic_error("Profile already finished");
    state_->measure_time();
    state_->data.success = success;
    state_->finished = true;
}

ProfileSnapshot ProfileReport::snapshot() const {
    std::lock_guard lock(state_->mutex);
    if (!state_->finished) state_->measure_time();
    return state_->data;
}

void ProfileReport::write_json(const std::filesystem::path& path) const {
    const auto data = snapshot();
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.imbue(std::locale::classic());
    output << std::setprecision(17) << std::boolalpha;
    output << "{\n  \"version\": 1,\n  \"scope\": "; quoted(output, data.scope);
    output << ",\n  \"requested_threads\": " << data.requested_threads;
    output << ",\n  \"thread_limit\": " << data.thread_limit;
    output << ",\n  \"wall_seconds\": " << data.wall_seconds;
    output << ",\n  \"cpu_seconds\": "; nullable(output, data.cpu_seconds);
    output << ",\n  \"process_peak_rss_bytes\": "; nullable(output, data.process_peak_rss_bytes);
    output << ",\n  \"success\": "; nullable(output, data.success);
    output << ",\n  \"io\": ";
    if (data.io) {
        const auto& io = *data.io;
        output << "{\"logical_bytes\":" << io.logical_bytes << ",\"decoded_bytes\":" << io.decoded_bytes
               << ",\"read_calls\":" << io.read_calls << ",\"read_samples\":" << io.read_samples << '}';
    } else output << "null";
    output << ",\n  \"cache\": ";
    if (data.cache) {
        const auto& cache = *data.cache;
        output << "{\"read_bytes\":" << cache.read_bytes << ",\"write_bytes\":" << cache.write_bytes
               << ",\"read_calls\":" << cache.read_calls << ",\"write_calls\":" << cache.write_calls
               << ",\"scenes\":" << cache.scenes << ",\"current_bytes\":" << cache.current_bytes
               << ",\"peak_bytes\":" << cache.peak_bytes << '}';
    } else output << "null";
    output << ",\n  \"shared_display_scale\": ";
    if (data.shared_display_scale) output << '[' << (*data.shared_display_scale)[0] << ',' << (*data.shared_display_scale)[1] << ']';
    else output << "null";
    output << ",\n  \"computed_samples\": "; nullable(output, data.computed_samples);
    output << ",\n  \"source_pixels\": "; nullable(output, data.source_pixels);
    output << ",\n  \"output\": ";
    if (data.output) output << "{\"width\":" << data.output->cols << ",\"height\":" << data.output->rows << '}';
    else output << "null";
    output << ",\n  \"source_pixels_per_second\": ";
    if (data.source_pixels && data.wall_seconds > 0) output << *data.source_pixels / data.wall_seconds;
    else output << "null";
    output << ",\n  \"output_pixels_per_second\": ";
    if (data.output && data.wall_seconds > 0) output << data.output->elements() / data.wall_seconds;
    else output << "null";
    output << ",\n  \"scratch_bytes\": "; nullable(output, data.scratch_bytes);
    output << ",\n  \"buffer_accounting\": \"explicit_reservations\",\n  \"encoder_internal_bytes\": null";
    output << ",\n  \"buffers\": ";
    if (data.buffers) {
        constexpr const char* names[]{"framebuffer", "ownership", "scene", "bins", "statistics", "worker_tiles", "encoder"};
        output << '{';
        for (std::size_t i = 0; i < data.buffers->size(); ++i) {
            if (i) output << ',';
            quoted(output, names[i]);
            output << ":{\"current_bytes\":" << (*data.buffers)[i].current_bytes
                   << ",\"peak_bytes\":" << (*data.buffers)[i].peak_bytes << '}';
        }
        output << '}';
    } else output << "null";
    output << ",\n  \"tracked_buffer_peak_bytes\": ";
    if (data.buffers) output << data.tracked_buffer_peak_bytes;
    else output << "null";
    output << ",\n  \"phase_times_may_overlap\": true,\n  \"phases\": {";
    bool first = true;
    for (const auto& [name, phase] : data.phases) {
        if (!first) output << ',';
        first = false;
        output << '\n'; quoted(output, name);
        output << ":{\"calls\":" << phase.calls << ",\"wall_seconds\":" << phase.wall_seconds << '}';
    }
    output << "\n  }\n}\n";
    output.close();
}

}
