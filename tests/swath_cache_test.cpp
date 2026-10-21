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
        SharedDisplayStatistics histogram("RY",context.profile()); histogram.add(input);
        const auto scale = histogram.scale();
        require(scale && scale->low == 0 && scale->high <= 0.98 && 0.98 - scale->high < 8.0 / 65535);
        SharedDisplayStatistics outside("RY",context.profile());
        std::fill(input.frame.planes[0].values.values().begin(),input.frame.planes[0].values.values().end(),8);
        outside.add(input); rejects([&] { outside.scale(); });
        input.frame.planes[0].values.values()[0] = -120;
        input.frame.planes[0].values.values()[1] = 80;
        SharedDisplayStatistics extrema("E",context.profile()); extrema.add(input);
        require(extrema.scale()->low == -105 && extrema.scale()->high == 40);
    }
    {
        PreparedProduct first,second;
        for (auto* product : {&first,&second})
            for (int channel = 0; channel < 3; ++channel)
                product->frame.planes.push_back({"1",Raster<float>({1,3},{0.2f,0.5f,0.8f}),
                                                Raster<std::uint8_t>({1,3},{1,1,1})});
        first.frame.planes[1].values.values()[0] = -0.5f;
        first.frame.planes[2].values.values()[2] = 1.5f;
        second.frame.planes[2].values.values()[2] = 2.5f;
        second.frame.planes[0].values.values()[1] = 100;
        second.frame.planes[1].values.values()[0] = -100;
        second.frame.planes[1].valid.values()[0] = 0;
        SharedDisplayStatistics shared("RT",context.profile());
        shared.add(first); shared.add(second);
        require(shared.scale()->low == -0.5 && shared.scale()->high == 2.5);
        SharedDisplayStatistics reversed("RT",context.profile());
        reversed.add(second); reversed.add(first);
        require(reversed.scale()->low == shared.scale()->low && reversed.scale()->high == shared.scale()->high);
        for (auto& plane : first.frame.planes)
            std::fill(plane.values.values().begin(),plane.values.values().end(),0.5f);
        SharedDisplayStatistics constant("RT",context.profile()); constant.add(first);
        require(constant.scale()->low == 0.5 && constant.scale()->high == 1.5);
        for (auto& plane : first.frame.planes)
            std::fill(plane.valid.values().begin(),plane.valid.values().end(),0);
        SharedDisplayStatistics empty("RT",context.profile()); empty.add(first);
        rejects([&] { empty.scale(); });
    }
    {
        PreparedProduct input;
        input.frame.planes.push_back({"1",Raster<float>({1,3},{0.05f,0.8f,0.8f}),Raster<std::uint8_t>({1,3},{1,1,1})});
        input.frame.planes.push_back({"1",Raster<float>({1,3},{0.1f,0.7f,0.7f}),Raster<std::uint8_t>({1,3},{1,1,1})});
        input.frame.planes.push_back({"1",Raster<float>({1,3},{0.15f,0.75f,0.75f}),Raster<std::uint8_t>({1,3},{1,1,1})});
        SharedDisplayStatistics statistics("RT",context.profile()); statistics.add(input);
        const auto scale = *statistics.scale();
        require(std::abs(scale.rgb_gain[0] - 0.7 / 0.75) < 1e-6 && scale.rgb_gain[1] == 1 &&
                std::abs(scale.rgb_gain[2] - 0.7 / 0.8) < 1e-6);
        require(std::abs(scale.low - 0.04375) < 1e-6 && std::abs(scale.high - 0.7) < 1e-6);
        const std::array<double,3> cloud{0.75,0.7,0.8};
        for (std::size_t channel = 0; channel < 3; ++channel)
            require(std::abs((cloud[channel] * scale.rgb_gain[channel] - scale.low) / (scale.high - scale.low) - 1) < 1e-6);
    }
    const auto report = context.profile().snapshot();
    require(report.cache && report.cache->scenes == 1 && report.cache->read_bytes > 0 && report.cache->write_bytes > 0 && !report.cache->current_bytes);
    for (auto usage : *report.buffers) require(!usage.current_bytes);
}
