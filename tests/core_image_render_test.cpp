/// SPDX-License-Identifier: MIT

#include "core/render.hpp"
#include "core/navigation.hpp"

#include <cmath>
#include <source_location>

namespace {

using namespace fy::core;

void require(bool condition,std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("Image raster check failed at line " + std::to_string(location.line()));
}

}

int main() {
    ExecutionContext context({.threads = 4});
    {
        Raster<float> plane({1,2},{0,1}); Raster<std::uint8_t> valid({1,2},{1,1});
        ImageRasterSource source{{1,2},{plane.view()},{valid.view()},{}};
        source.display.mapper = [](std::span<const float> values,const DisplayScale&) { return Color{values[0] * values[0],0,0}; };
        RasterOptions options;
        options.layout = LayoutTransform::make({1,1},{1,0,0,0,0}); options.keep_coverage = true;
        auto result = rasterize_image(source,options,context);
        require(std::get<Raster<Pixel8>>(result.pixels).values()[0].bgr[2] == 128);
        require(result.coverage->values()[0] == 1);
        valid.values()[1] = 0;
        auto masked = rasterize_image(source,options,context);
        require(masked.coverage->values()[0] == 0.5f);
        require(std::get<Raster<Pixel8>>(masked.pixels).values()[0].bgr[2] == 0);
    }
    {
        std::vector<float> samples(24,-9);
        std::vector<std::uint8_t> mask(24,0);
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t col = 0; col < 5; ++col) {
                samples[row * 8 + col] = float((row * 5 + col) % 7) / 6;
                mask[row * 8 + col] = (row != 1 || col != 2);
            }
        ImageRasterSource source{{3,5},{RasterView<const float>(samples,{3,5},8)},
                                  {RasterView<const std::uint8_t>(mask,{3,5},8)},{}};
        source.display.mapper = [](std::span<const float> values,const DisplayScale&) { return Color{values[0] * values[0],0.5,1 - values[0],0.75}; };
        RasterOptions options;
        options.layout = LayoutTransform::make({5,3},{3,1,2,1,2}); options.bit_depth = 16; options.keep_coverage = true;
        auto reference = rasterize_image(source,options,context);
        constexpr int density = 480;
        for (std::size_t row = 0; row < 5; ++row)
            for (std::size_t col = 0; col < 3; ++col) {
                std::array<double,3> sum{};
                double coverage = 0;
                for (int y = 0; y < density; ++y)
                    for (int x = 0; x < density; ++x) {
                        const auto r = std::size_t((row + (y + 0.5) / density) * 3 / 5);
                        const auto c = std::size_t((col + (x + 0.5) / density) * 5 / 3);
                        if (!mask[r * 8 + c]) continue;
                        const double value = samples[r * 8 + c];
                        sum[0] += (1 - value) * 0.75; sum[1] += 0.375; sum[2] += value * value * 0.75; coverage += 1;
                    }
                const auto actual = std::get<Raster<Pixel16>>(reference.pixels).view().at(options.layout.top + row,options.layout.left + col);
                for (std::size_t channel = 0; channel < 3; ++channel)
                    require(std::abs(int(actual.bgr[channel]) - int(std::lround(sum[channel] * 65535 / (density * density)))) <= 1);
                require(std::abs(reference.coverage->view().at(row,col) - coverage / (density * density)) < 1e-6);
            }
        for (int threads : {1,4,0}) {
            ExecutionContext executor({.threads = threads});
            for (std::size_t tile : {1,2,7}) {
                options.tile_size = tile;
                auto result = rasterize_image(source,options,executor);
                const auto& actual = std::get<Raster<Pixel16>>(result.pixels);
                const auto& expected = std::get<Raster<Pixel16>>(reference.pixels);
                for (std::size_t i = 0; i < actual.values().size(); ++i) require(actual.values()[i].bgr == expected.values()[i].bgr);
                for (std::size_t i = 0; i < result.coverage->values().size(); ++i)
                    require(result.coverage->values()[i] == reference.coverage->values()[i]);
            }
        }
        require(std::get<Raster<Pixel16>>(reference.pixels).view().at(0,0).bgr == std::array<std::uint16_t,3>{});
    }
    {
        NativeDiskGeometry disk;
        disk.shape = {7,9}; disk.scan_step_rad = {0.001,-0.001}; disk.scan_origin_rad = {-0.004,0.003};
        disk.longitude_deg = 179;
        for (double row : {0.,2.5,6.}) for (double col : {0.,4.,8.}) {
            const auto geographic = disk_geographic_at(disk,row,col);
            const auto image = disk_image_at(disk,geographic.latitude,geographic.longitude);
            require(geographic.valid && image.valid && std::abs(image.row - row) < 1e-10 && std::abs(image.col - col) < 1e-10);
        }
        require(!disk_image_at(disk,0,0).valid && !disk_image_at(disk,91,179).valid);
        const MapView view{{false,0,179},{175,183,-4,4},{10,10}};
        auto projected = project_disk(disk,view,context);
        require(projected.vertices.shape() == Shape{8,10});
        const auto position = disk_geographic_at(disk,-0.5,-0.5);
        const auto map = view.projection.project(position.latitude,position.longitude);
        require(std::abs(projected.vertices.view().at(0,0).x - float((map.x - 175) * 10 / 8)) < 1e-6);
    }
    for (const auto& buffer : *context.profile().snapshot().buffers) require(buffer.current_bytes == 0);
}
