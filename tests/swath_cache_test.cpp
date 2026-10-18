/// SPDX-License-Identifier: MIT

#include "products/cache.hpp"
#include "products/statistics.hpp"

#include <algorithm>
#include <fstream>
#include <source_location>

namespace {

using namespace fy::core;
using namespace fy::products;

void require(bool valid,std::source_location location = std::source_location::current()) {
    if (!valid) throw std::runtime_error("Cache check failed at line " + std::to_string(location.line()));
}

template<class F> void rejects(F&& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected cache failure");
}

LoadedProduct product(ExecutionContext& context,int shift) {
    const Shape shape{19,23};
    LoadedProduct result{context.profile().reserve(BufferKind::scene,shape.elements() * 24),{},
                          {0,std::size_t(shift ? 5 : 3),shape},{19,31}};
    SwathGeometry geometry{Raster<float>(shape),Raster<float>(shape),Raster<std::uint8_t>(shape)};
    for (std::size_t row = 0; row < shape.rows; ++row)
        for (std::size_t col = 0; col < shape.cols; ++col) {
            const auto i = row * shape.cols + col;
            geometry.latitude.values()[i] = float(row); geometry.longitude.values()[i] = float(100 + col);
            geometry.valid.values()[i] = !(row == 7 && col == 10);
        }
    result.product.frame.name = "cache";
    result.product.frame.geometry = std::make_shared<NativeGeometry>(std::move(geometry));
    for (int channel = 0; channel < 3; ++channel) {
        ScalarPlane plane{"1",Raster<float>(shape),Raster<std::uint8_t>(shape)};
        for (std::size_t i = 0; i < shape.elements(); ++i) {
            plane.values.values()[i] = float((i + channel * 3 + shift) % 19) / 18;
            plane.valid.values()[i] = i != 200 + static_cast<std::size_t>(channel);
        }
        result.product.frame.planes.push_back(std::move(plane));
    }
    result.product.display.mapper = [](std::span<const float> values,const DisplayScale&) {
        return Color{double(values[0]) * values[0],values[1],values[2]};
    };
    result.product.labels = {"file","date","time","satellite","product"};
    return result;
}

SwathRasterSource source(const LoadedProduct& product,const ProjectedSwath& projected,std::uint64_t order) {
    SwathRasterSource result;
    result.cells = product.source_window.shape; result.vertices = projected.vertices.view(); result.display = product.product.display;
    result.scene_order = order; result.source_column = product.source_window.col; result.source_columns = product.source_shape.cols;
    for (const auto& plane : product.product.frame.planes) {
        result.planes.push_back(plane.values.view()); result.valid.push_back(plane.valid.view());
    }
    return result;
}

}

int main() {
    ExecutionContext context({.threads = 4});
    {
        auto first = product(context,0),second = product(context,7);
        const MapView map{{false,0,110},{100,122,0,18},{24,32}};
        auto a = project_swath(std::get<SwathGeometry>(*first.product.frame.geometry),map,context);
        auto b = project_swath(std::get<SwathGeometry>(*second.product.frame.geometry),map,context);
        std::vector<SwathRasterSource> reference{source(first,a,0),source(second,b,2)};
        const auto prefix = context.scratch().path() / "cached";
        {
            CachedSwath cache(prefix,first,context,5);
            rejects([&] { cache.source(0); });
            cache.project(map);
            rejects([&] { cache.project(map); });
            std::vector<SwathRasterSource> cached{cache.source(0),reference[1]};
            RasterOptions options;
            options.layout = LayoutTransform::make(map.pixels,{3840,0,0,0,0});
            options.bit_depth = 16; options.keep_coverage = true; options.source_cache_bytes = 700;
            for (auto overlap : {OverlapPolicy::last_sample,OverlapPolicy::scan_center}) {
                options.overlap = overlap;
                auto expected = rasterize_swaths(reference,options,context);
                for (int threads : {1,4}) {
                    ExecutionContext worker({.threads = threads});
                    for (std::size_t tile : {3,8}) {
                        options.tile_size = tile;
                        auto actual = rasterize_swaths(cached,options,worker);
                        const auto pixels = std::get<Raster<Pixel16>>(actual.pixels).values();
                        const auto comparison = std::get<Raster<Pixel16>>(expected.pixels).values();
                        for (std::size_t i = 0; i < pixels.size(); ++i) {
                            require(pixels[i].bgr == comparison[i].bgr);
                            require(actual.coverage->values()[i] == expected.coverage->values()[i]);
                        }
                    }
                }
            }
            options.source_cache_bytes = 1;
            rejects([&] { rasterize_swaths(cached,options,context); });
            options.source_cache_bytes = 700;
            auto invalid = cached;
            invalid[0].read_block = [](Window) -> std::unique_ptr<SwathRasterBlock> { return {}; };
            rejects([&] { rasterize_swaths(invalid,options,context); });
            std::ofstream(prefix.string() + ".values",std::ios::binary | std::ios::trunc) << 'x';
            rejects([&] { rasterize_swaths(cached,options,context); });
        }
        require(!std::filesystem::exists(prefix.string() + ".values") && !std::filesystem::exists(prefix.string() + ".vertices"));
        const auto collision = context.scratch().path() / "collision";
        std::ofstream(collision.string() + ".geo") << "keep";
        rejects([&] { CachedSwath cache(collision,first,context); });
        require(std::filesystem::file_size(collision.string() + ".geo") == 4);
        std::filesystem::remove(collision.string() + ".geo");
    }
    {
        PreparedProduct input;
        input.frame.planes.push_back({"1",Raster<float>({1,100}),Raster<std::uint8_t>({1,100})});
        input.shared_percentile_plane = 0;
        for (std::size_t i = 0; i < 100; ++i) input.frame.planes[0].values.values()[i] = float(i) / 100;
        SharedDisplayStatistics histogram("RT",context.profile()); histogram.add(input);
        const auto scale = histogram.scale();
        require(scale && scale->low == 0 && scale->high <= 0.98 && 0.98 - scale->high < 8.0 / 65535);
        SharedDisplayStatistics outside("RT",context.profile());
        std::fill(input.frame.planes[0].values.values().begin(),input.frame.planes[0].values.values().end(),8);
        outside.add(input); rejects([&] { outside.scale(); });
        input.frame.planes[0].values.values()[0] = -120;
        input.frame.planes[0].values.values()[1] = 80;
        SharedDisplayStatistics extrema("E",context.profile()); extrema.add(input);
        require(extrema.scale()->low == -105 && extrema.scale()->high == 40);
    }
    const auto report = context.profile().snapshot();
    require(report.cache && report.cache->scenes == 1 && report.cache->read_bytes > 0 && report.cache->write_bytes > 0 && !report.cache->current_bytes);
    for (auto usage : *report.buffers) require(!usage.current_bytes);
}
