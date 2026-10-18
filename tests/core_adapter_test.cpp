/// SPDX-License-Identifier: MIT

#include "core/adapter.hpp"

#include <atomic>
#include <thread>

namespace {

using namespace fy::core;

void require(bool condition) {
    if (!condition) throw std::runtime_error("Adapter check failed");
}

template<class F>
void rejects(F&& action) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Expected adapter failure");
}

struct Trace {
    std::thread::id io_thread;
    std::atomic<int> wrong_thread{0};
    std::atomic<int> destroyed{0};
    std::atomic<int> reads{0};
    std::atomic<bool> fail_read{false};
    std::atomic<bool> wrong_window{false};
    std::atomic<bool> wrong_geometry{false};
    bool invalid_metadata = false;
    void check() {
        if (std::this_thread::get_id() != io_thread) ++wrong_thread;
    }
};

class SyntheticAdapter final : public SensorAdapter {
public:
    explicit SyntheticAdapter(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) { trace_->check(); }
    ~SyntheticAdapter() override { trace_->check(); ++trace_->destroyed; }

    SensorMetadata metadata() const override {
        trace_->check();
        SensorMetadata result;
        result.satellite = "synthetic"; result.sensor = "custom"; result.resolution = "1km";
        result.grids.push_back({.id = "native", .shape = trace_->invalid_metadata ? Shape{} : Shape{8, 10}});
        return result;
    }

    std::vector<BandDescriptor> bands() const override {
        trace_->check();
        BandDescriptor band;
        band.id = 7; band.grid = "native"; band.dataset = "samples"; band.unit = "K";
        return {band};
    }

    SampleBlock read_window(int band, Window window) override {
        trace_->check(); ++trace_->reads;
        if (trace_->fail_read) throw std::runtime_error("Synthetic read failure");
        SampleBlock result{band, window, Raster<float>(window.shape), Raster<std::uint8_t>(window.shape)};
        for (std::size_t r = 0; r < window.shape.rows; ++r)
            for (std::size_t c = 0; c < window.shape.cols; ++c) {
                result.values.view().at(r, c) = static_cast<float>((window.row + r) * 10 + window.col + c);
                result.valid.view().at(r, c) = 1;
            }
        if (trace_->wrong_window) ++result.window.row;
        return result;
    }

    NativeGeometry read_geometry(const std::string&, Window window) override {
        trace_->check();
        if (trace_->wrong_geometry) window.shape = {1, 1};
        return SwathGeometry{Raster<float>(window.shape), Raster<float>(window.shape),
                             Raster<std::uint8_t>(window.shape)};
    }

private:
    std::shared_ptr<Trace> trace_;
};

}

int main() {
    auto channel = std::make_shared<IoChannel>();
    auto trace = std::make_shared<Trace>();
    trace->io_thread = channel->run([] { return std::this_thread::get_id(); });
    require(trace->io_thread != std::this_thread::get_id());
    require(channel->run([&] { return channel->run([] { return 42; }); }) == 42);
    require(channel->run([&] { return channel->submit([] { return 44; }).get(); }) == 44);
    rejects([&] { channel->run([] { throw std::runtime_error("I/O error"); }); });
    require(channel->run([] { return 43; }) == 43);
    AdapterRegistry registry;
    auto factory = [trace](const SourceFiles&) { return std::make_unique<SyntheticAdapter>(trace); };
    registry.add("new-payload", factory);
    rejects([&] { registry.add("new-payload", factory); });
    rejects([&] { registry.open("missing", {}, channel); });
    {
        auto session = registry.open("new-payload", {}, channel);
        require(session.metadata().sensor == "custom");
        require(session.bands()[0].id == 7);
        auto moved = std::move(session);
        rejects([&] { session.metadata(); });
        auto block = moved.read_window(7, {2, 3, {2, 4}});
        require(block.values.view().at(0, 0) == 23 && block.values.view().at(1, 3) == 36);
        auto geometry = moved.read_geometry("native", {2, 3, {2, 4}});
        require(geometry_shape(geometry) == Shape{2, 4});
        const auto read_count = trace->reads.load();
        rejects([&] { moved.read_window(99, {0, 0, {1, 1}}); });
        rejects([&] { moved.read_window(7, {7, 9, {2, 2}}); });
        require(trace->reads == read_count);
        trace->fail_read = true;
        rejects([&] { moved.read_window(7, {0, 0, {1, 1}}); });
        trace->fail_read = false;
        trace->wrong_window = true;
        rejects([&] { moved.read_window(7, {0, 0, {1, 1}}); });
        trace->wrong_window = false;
        trace->wrong_geometry = true;
        rejects([&] { moved.read_geometry("native", {0, 0, {2, 2}}); });
        trace->wrong_geometry = false;
        std::vector<std::future<void>> consumers;
        for (std::size_t row = 0; row < 8; ++row)
            consumers.push_back(std::async(std::launch::async, [&, row] {
                auto samples = moved.read_window(7, {row, 0, {1, 10}});
                require(samples.values.view().at(0, 9) == row * 10 + 9);
            }));
        for (auto& consumer : consumers) consumer.get();
        moved = registry.open("new-payload", {}, channel);
        require(trace->destroyed == 1);
    }
    require(trace->destroyed == 2 && trace->wrong_thread == 0);
    trace->invalid_metadata = true;
    rejects([&] { registry.open("new-payload", {}, channel); });
    require(trace->destroyed == 3 && trace->wrong_thread == 0);
    std::future<int> drained;
    {
        IoChannel temporary;
        drained = temporary.submit([] { return 45; });
    }
    require(drained.get() == 45);
}
