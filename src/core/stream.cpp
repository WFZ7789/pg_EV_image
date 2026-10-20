/// SPDX-License-Identifier: MIT

#include "core/stream.hpp"

#include <algorithm>

namespace fy::core {

void stream_calibrated(SensorSession& session, int band, Window region, ExecutionContext& context,
                        const std::function<void(SampleTile&&)>& consume, std::size_t block_rows,
                        InvalidSamplePolicy invalid,std::optional<CalibrationDescriptor> calibration_override) {
    if (!consume || !region.shape.elements())
        throw std::invalid_argument("Invalid streaming request");
    if (!block_rows) block_rows = session.preferred_block_rows(band,region);
    const auto bands = session.bands();
    const auto found = std::ranges::find(bands, band, &BandDescriptor::id);
    if (found == bands.end()) throw std::invalid_argument("Unknown streaming band");
    const auto descriptor = *found;
    region.validate(session.metadata().grid(descriptor.grid).shape);
    struct Pending {
        std::future<SampleTile> result;
        ~Pending() { if (result.valid()) result.wait(); }
    } pending;
    auto schedule = [&](std::size_t row) {
        Window window{region.row + row, region.col, {std::min(block_rows, region.shape.rows - row), region.shape.cols}};
        const auto count = window.shape.elements();
        if (count > std::numeric_limits<std::uint64_t>::max() / (sizeof(float) + sizeof(std::uint8_t)))
            throw std::overflow_error("Streaming buffer size overflow");
        auto memory = context.profile().reserve(BufferKind::scene, count * (sizeof(float) + sizeof(std::uint8_t)));
        return context.io().submit([&session, &context, band, window, memory = std::move(memory)]() mutable {
            auto reserved = std::move(memory);
            auto phase = context.profile().phase("read");
            return SampleTile{session.read_window(band, window), std::move(reserved)};
        });
    };
    pending.result = schedule(0);
    std::size_t row = 0;
    while (row < region.shape.rows) {
        auto tile = pending.result.get();
        const auto rows = tile.samples.window.shape.rows;
        if (rows < region.shape.rows - row) pending.result = schedule(row + rows);
        calibrate(tile.samples,calibration_override ? *calibration_override : descriptor.calibration,context,invalid);
        {
            auto phase = context.profile().phase("consume");
            consume(std::move(tile));
        }
        row += rows;
    }
}

}
