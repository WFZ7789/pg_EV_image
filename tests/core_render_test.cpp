/// SPDX-License-Identifier: MIT

#include "core/render.hpp"

#include <algorithm>
#include <cmath>
#include <source_location>

namespace {

using namespace fy::core;

void require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("Raster check failed at line " + std::to_string(location.line()));
}

struct OwnedSource {
    Raster<PixelPoint> vertices;
    Raster<float> values;
    Raster<std::uint8_t> valid;
    Color color;
    std::uint64_t order = 0;

    SwathRasterSource view() const {
        DisplayDescriptor display;
        display.mapper = [color = color](std::span<const float>, const DisplayScale&) { return color; };
        return {values.shape(), vertices.view(), {values.view()}, {valid.view()}, std::move(display), order};
    }
};

OwnedSource quad(std::array<PixelPoint,4> vertices, Color color, std::uint64_t order = 0) {
    return {Raster<PixelPoint>({2,2},{vertices[0],vertices[1],vertices[3],vertices[2]}),
            Raster<float>({1,1},{1}),Raster<std::uint8_t>({1,1},{1}),color,order};
}

OwnedSource rectangle(float left, float top, float right, float bottom, Color color, std::uint64_t order = 0) {
    return quad({PixelPoint{left,top},{right,top},{right,bottom},{left,bottom}},color,order);
}

RasterOptions options(Shape map, int bits = 8, std::size_t tile = 64) {
    RasterOptions result;
    result.layout = LayoutTransform::make(map, {3840,0,0,0,0});
    result.bit_depth = bits; result.tile_size = tile; result.keep_coverage = true;
    return result;
}

bool contains(const std::array<PixelPoint,4>& polygon, double x, double y) {
    bool positive = false, negative = false;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto a = polygon[i], b = polygon[(i + 1) % 4];
        const auto value = (double(b.x) - a.x) * (y - a.y) - (double(b.y) - a.y) * (x - a.x);
        positive = positive || value > 0;
        negative = negative || value < 0;
    }
    return !(positive && negative);
}

}

int main() {
    ExecutionContext context({.threads = 4});
    {
        auto wrapped = rectangle(-179,0,179,1,{1,0,0});
        auto source = wrapped.view(); source.horizontal_period_pixels = 360;
        auto result = rasterize_swaths(std::span(&source,1),options({1,1}),context);
        require(result.coverage->values()[0] == 0);
        require(std::get<Raster<Pixel8>>(result.pixels).values()[0].bgr == std::array<std::uint8_t,3>{});
    }
    {
        auto a = rectangle(0,0,0.5,1,{0,0,0});
        auto b = rectangle(0.5,0,1,1,{0,0,0},1);
        a.values.values()[0] = 0.2f; b.values.values()[0] = 0.8f;
        std::vector<SwathRasterSource> sources{a.view(),b.view()};
        for (auto& source : sources)
            source.display.mapper = [](std::span<const float> value, const DisplayScale&) {
                return Color{double(value[0]) * value[0],0,0};
            };
        auto result = rasterize_swaths(sources,options({1,1}),context);
        const auto pixel = std::get<Raster<Pixel8>>(result.pixels).view().at(0,0).bgr;
        require(pixel[2] == 87 && pixel[0] == 0 && pixel[1] == 0);
        require(result.coverage->values()[0] == 1);
    }
    {
        auto background = rectangle(0,0,1,1,{0,0,1},1);
        auto foreground = rectangle(0,0,0.25,1,{1,0,0},2);
        std::vector<SwathRasterSource> sources{background.view(),foreground.view()};
        for (int bits : {8,16}) {
            auto result = rasterize_swaths(sources,options({1,1},bits),context);
            if (bits == 8) require(std::get<Raster<Pixel8>>(result.pixels).values()[0].bgr == std::array<std::uint8_t,3>{191,0,64});
            else require(std::get<Raster<Pixel16>>(result.pixels).values()[0].bgr == std::array<std::uint16_t,3>{49151,0,16384});
        }
        auto center = options({1,1}); center.overlap = OverlapPolicy::scan_center;
        sources[0].source_column = 3; sources[0].source_columns = 7;
        sources[1].source_column = 1; sources[1].source_columns = 7;
        auto centered = rasterize_swaths(sources,center,context);
        require(std::get<Raster<Pixel8>>(centered.pixels).values()[0].bgr == std::array<std::uint8_t,3>{255,0,0});
        sources[0].source_column = 1;
        sources[0].scene_order = 5; sources[1].scene_order = 4;
        std::reverse(sources.begin(),sources.end());
        auto tied = rasterize_swaths(sources,center,context);
        require(std::get<Raster<Pixel8>>(tied.pixels).values()[0].bgr == std::array<std::uint8_t,3>{191,0,64});
    }
    {
        auto diamond = quad({PixelPoint{0.5,0},{1,0.5},{0.5,1},{0,0.5}},{1,1,1});
        const std::vector<SwathRasterSource> sources{diamond.view()};
        auto result = rasterize_swaths(sources,options({1,1},16),context);
        require(std::get<Raster<Pixel16>>(result.pixels).values()[0].bgr[0] == 32768);
        require(result.coverage->values()[0] == 0.5f);
        auto thin = rectangle(0.499f,0,0.501f,1,{1,0,0});
        const std::vector<SwathRasterSource> narrow{thin.view()};
        auto line = rasterize_swaths(narrow,options({1,1},16),context);
        require(std::get<Raster<Pixel16>>(line.pixels).values()[0].bgr[2] == 131);
    }
    {
        OwnedSource source{Raster<PixelPoint>({2,4},{{0,0},{1,0},{0,0},{1,0},{0,1},{1,1},{0,1},{1,1}}),
                            Raster<float>({1,3},{0.1f,0.5f,0.9f}),Raster<std::uint8_t>({1,3},{1,1,1}),{}};
        auto view = source.view();
        view.display.mapper = [](std::span<const float> value,const DisplayScale&) { return Color{value[0],value[0],value[0]}; };
        const std::vector<SwathRasterSource> sources{view};
        auto last = rasterize_swaths(sources,options({1,1}),context);
        require(std::get<Raster<Pixel8>>(last.pixels).values()[0].bgr[0] == 229);
        auto opt = options({1,1}); opt.overlap = OverlapPolicy::scan_center;
        auto center = rasterize_swaths(sources,opt,context);
        require(std::get<Raster<Pixel8>>(center.pixels).values()[0].bgr[0] == 128);
    }
    {
        OwnedSource source{Raster<PixelPoint>({3,4},{{-0.2f,-0.1f},{5.3f,-0.1f},{11.8f,-0.1f},{17.4f,-0.1f},
                                                    {-0.2f,4.7f},{5.3f,4.7f},{11.8f,4.7f},{17.4f,4.7f},
                                                    {-0.2f,9.5f},{5.3f,9.5f},{11.8f,9.5f},{17.4f,9.5f}}),
                            Raster<float>({2,3},{1,0.1f,0.7f,0.4f,0.9f,0.2f}),Raster<std::uint8_t>({2,3},{1,0,1,1,1,1}),{}};
        auto view = source.view();
        view.display.mapper = [](std::span<const float> value,const DisplayScale&) { return Color{value[0],value[0],value[0]}; };
        const std::vector<SwathRasterSource> sources{view};
        auto reference = rasterize_swaths(sources,options({9,17},16),context);
        for (int threads : {1,4,0}) {
            ExecutionContext executor({.threads = threads});
            for (std::size_t tile : {1,3,8,64}) {
                auto result = rasterize_swaths(sources,options({9,17},16,tile),executor);
                const auto& actual = std::get<Raster<Pixel16>>(result.pixels);
                const auto& expected = std::get<Raster<Pixel16>>(reference.pixels);
                for (std::size_t i = 0; i < actual.values().size(); ++i) {
                    require(actual.values()[i].bgr == expected.values()[i].bgr);
                    require(result.coverage->values()[i] == reference.coverage->values()[i]);
                }
            }
        }
        require(reference.coverage->view().at(1,8) == 0);
        require(reference.coverage->view().at(7,8) == 1);
    }
    {
        const std::array<PixelPoint,4> red{{{0.2f,0.3f},{3.5f,0.8f},{2.9f,3.7f},{-0.1f,3.1f}}};
        const std::array<PixelPoint,4> green{{{1.2f,-1},{1.27f,-1},{2.1f,5},{2.03f,5}}};
        auto a = rectangle(0,0,4,4,{0.2,0.2,0.2});
        auto b = quad(red,{1,0,0},1);
        auto c = quad(green,{0,1,0},2);
        const std::vector<SwathRasterSource> sources{a.view(),b.view(),c.view()};
        auto result = rasterize_swaths(sources,options({4,4},8,3),context);
        constexpr int samples = 512;
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t col = 0; col < 4; ++col) {
                std::array<double,3> sum{};
                for (int y = 0; y < samples; ++y)
                    for (int x = 0; x < samples; ++x) {
                        const auto px = col + (x + 0.5) / samples, py = row + (y + 0.5) / samples;
                        if (contains(green,px,py)) sum[1] += 1;
                        else if (contains(red,px,py)) sum[2] += 1;
                        else for (auto& channel : sum) channel += 0.2;
                    }
                const auto pixel = std::get<Raster<Pixel8>>(result.pixels).view().at(row,col).bgr;
                for (std::size_t i = 0; i < 3; ++i)
                    require(std::abs(int(pixel[i]) - int(std::lround(sum[i] * 255 / (samples * samples)))) <= 1);
                require(std::abs(result.coverage->view().at(row,col) - 1) < 1e-6);
            }
    }
    {
        auto large = rectangle(0,0,8,8,{1,0,0});
        const std::vector<SwathRasterSource> sources{large.view()};
        auto configured = options({8,8});
        configured.exact_coverage = false;
        configured.keep_coverage = false;
        auto result = rasterize_swaths(sources,configured,context);
        for (const auto pixel : std::get<Raster<Pixel8>>(result.pixels).values())
            require(pixel.bgr == std::array<std::uint8_t,3>{0,0,255});
        require(!result.coverage);
    }
    {
        auto large = quad({PixelPoint{0.2f,0.3f},{9.6f,1.4f},{10.4f,10.1f},{0.9f,9.4f}},{1,1,1});
        const std::vector<SwathRasterSource> sources{large.view()};
        auto reference = rasterize_swaths(sources,options({12,12},16),context);
        for (std::size_t tile : {3,64}) {
            auto configured = options({12,12},16,tile);
            configured.exact_coverage = false;
            auto result = rasterize_swaths(sources,configured,context);
            for (std::size_t i = 0; i < 144; ++i) {
                require(std::abs(result.coverage->values()[i] - reference.coverage->values()[i]) < 1e-6);
                require(std::get<Raster<Pixel16>>(result.pixels).values()[i].bgr ==
                        std::get<Raster<Pixel16>>(reference.pixels).values()[i].bgr);
            }
        }
    }
    {
        OwnedSource mixed{Raster<PixelPoint>({4,4}),Raster<float>({3,3}),Raster<std::uint8_t>({3,3}),{1,1,1}};
        const std::array<float,4> x{-0.2f,3.7f,7.8f,12.2f},y{-0.1f,4,7.9f,12.1f};
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t col = 0; col < 4; ++col) mixed.vertices.view().at(row,col) = {x[col],y[row]};
        std::fill(mixed.valid.values().begin(),mixed.valid.values().end(),1);
        const std::vector<SwathRasterSource> sources{mixed.view()};
        for (std::size_t tile : {3,64}) {
            auto configured = options({12,12},16,tile);
            configured.exact_coverage = false;
            auto result = rasterize_swaths(sources,configured,context);
            for (std::size_t i = 0; i < 144; ++i) {
                require(std::abs(result.coverage->values()[i] - 1) < 1e-6);
                require(std::get<Raster<Pixel16>>(result.pixels).values()[i].bgr ==
                        std::array<std::uint16_t,3>{65535,65535,65535});
            }
        }
    }
    {
        OwnedSource dense{Raster<PixelPoint>({129,129}),Raster<float>({128,128}),Raster<std::uint8_t>({128,128}),{1,1,1}};
        for (std::size_t row = 0; row <= 128; ++row)
            for (std::size_t col = 0; col <= 128; ++col)
                dense.vertices.view().at(row,col) = {float(col) / 32,float(row) / 32};
        for (std::size_t row = 0; row < 128; ++row)
            for (std::size_t col = 0; col < 128; ++col) {
                dense.values.view().at(row,col) = 100;
                dense.valid.view().at(row,col) = !(row % 32 == 15 && col % 32 == 15);
            }
        const std::vector<SwathRasterSource> sources{dense.view()};
        for (int threads : {1,4}) {
            ExecutionContext executor({.threads = threads});
            for (bool exact : {true,false}) {
                auto configured = options({4,4},16,2);
                configured.exact_coverage = exact;
                auto result = rasterize_swaths(sources,configured,executor);
                for (auto pixel : std::get<Raster<Pixel16>>(result.pixels).values())
                    require(pixel.bgr == std::array<std::uint16_t,3>{65471,65471,65471});
                for (auto coverage : result.coverage->values()) require(coverage == 1023.0f / 1024);
            }
        }
    }
    {
        SwathGeometry geometry{Raster<float>({48,8}),Raster<float>({48,8}),Raster<std::uint8_t>({48,8}),24,0};
        std::fill(geometry.valid.values().begin(),geometry.valid.values().end(),1);
        for (std::size_t row = 0; row < 48; ++row)
            for (std::size_t col = 0; col < 8; ++col) {
                const double y = row + .5 - (row/24)*.8;
                geometry.latitude.view().at(row,col) = float(60-y*.01);
                geometry.longitude.view().at(row,col) = float(120+(col+.5+y*.1)*.01);
            }
        const MapView map{{false,0,120},{120,120.16,59.52,60},{48,16}};
        std::vector<ProjectedSwath> projected;
        projected.reserve(2);
        Raster<float> values({24,8}); Raster<std::uint8_t> valid({24,8});
        std::fill(values.values().begin(),values.values().end(),1);
        std::fill(valid.values().begin(),valid.values().end(),1);
        std::vector<SwathRasterSource> sources;
        for (std::size_t row : {0,24}) {
            auto strip = subset_swath_geometry(geometry,{row,0,{24,8}});
            projected.push_back(project_swath(strip,map,context));
            DisplayDescriptor display;
            display.mapper = [](std::span<const float>,const DisplayScale&) { return Color{1,1,1}; };
            sources.push_back({{24,8},projected.back().vertices.view(),{values.view()},{valid.view()},std::move(display),row});
        }
        auto configured = options(map.pixels);
        configured.overlap = OverlapPolicy::scan_center;
        auto result = rasterize_swaths(sources,configured,context);
        for (std::size_t row = 22; row <= 25; ++row)
            for (std::size_t col = 5; col <= 7; ++col) {
                require(result.coverage->view().at(row,col) > .9999f);
                require(std::get<Raster<Pixel8>>(result.pixels).view().at(row,col).bgr ==
                        std::array<std::uint8_t,3>{255,255,255});
            }
        Raster<std::uint8_t> missing_valid({24,8});
        sources[1].valid = {missing_valid.view()};
        auto missing = rasterize_swaths(sources,configured,context);
        require(missing.coverage->view().at(10,6) > .9999f);
        require(missing.coverage->view().at(26,6) == 0);
    }
    {
        SwathGeometry geometry{Raster<float>({3,3},{0,0,0,1,1,1,2,2,2}),
                               Raster<float>({3,3},{0,1,2,0,1,2,0,1,2}),Raster<std::uint8_t>({3,3},{1,1,1,1,1,1,1,1,1})};
        MapView map{{false,0,0},{0,2,0,2},{4,4}};
        auto projected = project_swath(geometry,map,context);
        require(projected.vertices.view().at(1,1).x == 1 && projected.vertices.view().at(1,1).y == 3);
        require(geometry.latitude.values()[0] == 0 && geometry.longitude.values()[8] == 2);
        geometry.valid.view().at(1,1) = 0;
        auto masked = project_swath(geometry,map,context);
        require(std::isnan(masked.vertices.view().at(1,1).x));
        require(std::isfinite(masked.vertices.view().at(0,0).x));
        MapProjection globe{true,0,0};
        require(!globe.project(0,180).valid);
        MapProjection dateline{false,0,180};
        require(dateline.project(10,-179).x == 181 && dateline.project(10,179).x == 179);
    }
    const auto snapshot = context.profile().snapshot();
    for (const auto& usage : *snapshot.buffers) require(usage.current_bytes == 0);
}
