/// SPDX-License-Identifier: MIT

#include "core/stream.hpp"

#include <atomic>

namespace {

using namespace fy::core;

void require(bool condition) {
    if (!condition) throw std::runtime_error("Streaming check failed");
}

struct Control {
    std::promise<void> second_started;
    std::promise<void> release_second;
    std::shared_future<void> release = release_second.get_future().share();
    bool fail_second = false;
    std::atomic<bool> gated{false};
    std::atomic<int> completed_reads{0};
};

class Adapter final : public SensorAdapter {
public:
    explicit Adapter(std::shared_ptr<Control> control) : control_(std::move(control)) {}
    SensorMetadata metadata() const override {
        SensorMetadata result;
        result.satellite = "synthetic"; result.sensor = "stream"; result.resolution = "1km";
        result.grids.push_back({.id = "native", .shape = {600,4}});
        return result;
    }
    std::vector<BandDescriptor> bands() const override {
        BandDescriptor band;
        band.id = 3; band.dataset = "samples"; band.grid = "native"; band.unit = "DN";
        band.calibration.kind = CalibrationKind::linear;
        band.calibration.coefficients = {2,1};
        return {band};
    }
    SampleBlock read_window(int band, Window window) override {
        if (window.row == 256 && !control_->gated.exchange(true)) {
            control_->second_started.set_value();
            control_->release.wait();
            if (control_->fail_second) throw std::runtime_error("read failure");
        }
        SampleBlock result{band, window, Raster<float>(window.shape), Raster<std::uint8_t>(window.shape)};
        for (std::size_t row = 0; row < window.shape.rows; ++row)
            for (std::size_t col = 0; col < window.shape.cols; ++col) {
                result.values.view().at(row, col) = static_cast<float>((window.row + row) * 4 + window.col + col);
                result.valid.view().at(row, col) = 1;
            }
        ++control_->completed_reads;
        return result;
    }
    NativeGeometry read_geometry(const std::string&, Window) override { throw std::logic_error("unused"); }
private:
    std::shared_ptr<Control> control_;
};

}

int main() {
    ExecutionContext context({.threads = 4});
    for (int failure : {0, 1, 2}) {
        auto control = std::make_shared<Control>();
        control->fail_second = failure == 2;
        auto started = control->second_started.get_future();
        AdapterRegistry registry;
        registry.add("stream", [control](const SourceFiles&) { return std::make_unique<Adapter>(control); });
        auto session = registry.open("stream", {});
        std::optional<SampleTile> retained;
        std::size_t rows = 0;
        bool failed = false;
        try {
            stream_calibrated(session, 3, {0,0,{600,4}}, context, [&](SampleTile&& tile) {
                if (!rows) {
                    const auto status = started.wait_for(std::chrono::seconds(2));
                    control->release_second.set_value();
                    require(status == std::future_status::ready);
                    if (failure == 1) throw std::runtime_error("consumer failure");
                }
                for (std::size_t row = 0; row < tile.samples.window.shape.rows; ++row)
                    for (std::size_t col = 0; col < 4; ++col)
                        require(tile.samples.values.view().at(row, col) == (rows + row) * 8 + col * 2 + 1);
                rows += tile.samples.window.shape.rows;
                if (failure == 0 && !retained) retained.emplace(std::move(tile));
            });
        } catch (const std::runtime_error&) { failed = true; }
        if (!failure) {
            require(!failed && rows == 600 && retained.has_value());
            require(retained->samples.values.view().at(0,0) == 1);
            const auto memory = context.profile().snapshot();
            require((*memory.buffers)[static_cast<std::size_t>(BufferKind::scene)].current_bytes == 256 * 4 * 5);
        } else {
            require(failed && rows == (failure == 1 ? 0 : 256));
            require(control->completed_reads == (failure == 1 ? 2 : 1));
        }
        retained.reset();
        require((*context.profile().snapshot().buffers)[static_cast<std::size_t>(BufferKind::scene)].current_bytes == 0);
        require(session.read_window(3, {0,0,{1,1}}).band == 3);
    }
}
