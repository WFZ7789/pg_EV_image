/// SPDX-License-Identifier: MIT

#include "core/model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace {

void require(bool condition) {
    if (!condition) throw std::runtime_error("Core model check failed");
}

template<class F>
void rejects(F&& action) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Expected invalid contract to be rejected");
}

}

int main() {
    using namespace fy::core;
    static_assert(!std::is_copy_constructible_v<Raster<float>>);
    static_assert(!std::is_copy_assignable_v<Raster<float>>);
    static_assert(std::is_nothrow_move_constructible_v<Raster<float>>);
    static_assert(!std::is_convertible_v<RasterView<const float>, RasterView<float>>);
    Raster<float> raster({3, 5}, std::vector<float>{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14});
    auto* original = raster.values().data();
    auto window = raster.view().window({1, 2, {2, 2}});
    require(window.stride() == 5 && window.storage().size() == 7);
    require(window.at(0, 0) == 7 && window.at(1, 1) == 13);
    window.at(0, 1) = 80;
    RasterView<const float> borrowed(window);
    Raster<float> owner(std::move(raster));
    require(owner.values().data() == original && owner.values()[8] == 80);
    require(raster.shape() == Shape{} && raster.values().empty());
    require(borrowed.at(0, 1) == 80);
    rejects([&] { window.at(2, 0); });
    rejects([&] { window.at(0, 2); });
    rejects([&] { window.window({1, 1, {2, 2}}); });
    rejects([&] { RasterView<float>(owner.values(), {3, 5}, 4); });
    rejects([&] { RasterView<float>(owner.values().first(3), {2, 2}, 2); });
    rejects([] { Shape{0, 3}.elements(); });
    rejects([] { Shape{std::numeric_limits<std::size_t>::max(), 2}.elements(); });
    rejects([&] { Window{std::numeric_limits<std::size_t>::max(), 0, {1, 1}}.validate(owner.shape()); });
    require(owner.view().window({3, 5, {}}).shape() == Shape{});

    SwathGeometry geometry{Raster<float>({1, 3}, {12, -9999.9f, -12}),
                           Raster<float>({1, 3}, {179, -9999.9f, -179}),
                           Raster<std::uint8_t>({1, 3}, {1, 0, 1})};
    geometry.validate();
    geometry.valid.values()[1] = 1;
    rejects([&] { geometry.validate(); });
    geometry.valid.values()[1] = 0;
    auto native = std::make_shared<const NativeGeometry>(std::move(geometry));
    ProductFrame frame;
    frame.name = "scalar";
    frame.geometry = native;
    frame.planes.push_back({"K", Raster<float>({1, 3}, {250, 270, NAN}),
                           Raster<std::uint8_t>({1, 3}, {1, 1, 0})});
    frame.validate();
    const auto& geo = std::get<SwathGeometry>(*native);
    SwathTileView tile{{0, 0, {1, 3}}, geo.latitude.view(), geo.longitude.view(), geo.valid.view(),
                      {frame.planes[0].values.view()}, {frame.planes[0].valid.view()}};
    tile.validate();
    require(tile.location_valid.at(0, 1) == 0 && tile.value_valid[0].at(0, 1) == 1);
    require(tile.location_valid.at(0, 2) == 1 && tile.value_valid[0].at(0, 2) == 0);
    frame.planes[0].valid.values()[2] = 1;
    rejects([&] { frame.validate(); });
    frame.planes[0].valid.values()[2] = 0;
    std::fill(frame.planes[0].valid.values().begin(), frame.planes[0].valid.values().end(), 0);
    frame.validate();
    tile.source_window.shape = {1, 2};
    rejects([&] { tile.validate(); });

    SensorMetadata metadata;
    metadata.satellite = "synthetic"; metadata.sensor = "test"; metadata.resolution = "1km";
    metadata.grids.push_back({.id = "native", .shape = {1, 3}});
    BandDescriptor band;
    band.id = 24; band.grid = "native"; band.dataset = "measurements"; band.unit = "K";
    std::vector<BandDescriptor> bands{band};
    ProductPlan plan;
    plan.mode = "E"; plan.calculation_grid = "native";
    plan.inputs.push_back({24, "native", 1, 0, true, true});
    plan.frames.push_back({.name = "E", .units = {"K"}, .bit_depth = 16});
    StatisticsSpec statistics;
    statistics.domain = StatisticsDomain::task;
    statistics.rank = RankRule::histogram_ceiling_rank;
    statistics.exclude_nan_only = false;
    statistics.histogram_range = std::array<double, 2>{0, 8};
    statistics.histogram_bins = 65536;
    plan.statistics.push_back(statistics);
    plan.validate(metadata, bands);
    plan.inputs[0].band = 99;
    rejects([&] { plan.validate(metadata, bands); });
    plan.inputs[0].band = 24;
    plan.statistics[0].histogram_bins = 0;
    rejects([&] { plan.validate(metadata, bands); });
    plan.statistics.clear();
    plan.inputs.clear();
    plan.validate(metadata, bands);
    plan.statistics.push_back({});
    plan.statistics[0].sample_divisor = 0;
    rejects([&] { plan.validate(metadata, bands); });
    plan.statistics[0].sample_divisor = 1;
    plan.statistics[0].upper_cutoff = NAN;
    rejects([&] { plan.validate(metadata, bands); });
    metadata.grids[0].paired_grid = "missing";
    rejects([&] { metadata.validate(); });

    auto layout = LayoutTransform::make({2000, 2400});
    require(layout.framebuffer == Shape{2202, 2588});
    require(layout.top == 101 && layout.left == 0 && layout.scale == 0.625);
    auto anchor = layout.map_anchor(100, 80);
    require(anchor[0] == 62.5 && anchor[1] == 151);
    rejects([] { LayoutTransform::make({2000, 2400}, {0, 0, 162, 300, 162}); });
    rejects([] { LayoutTransform::make({2000, 2400}, {3840, 0, NAN, 300, 162}); });

    NativeDiskGeometry disk;
    disk.shape = {16, 16};
    disk.scan_step_rad = {0.0001, -0.0001};
    disk.validate();
    disk.satellite_distance_m = disk.equatorial_radius_m;
    rejects([&] { disk.validate(); });
    ObservationGeometry observations{
        {Raster<float>({1, 1}, {12}), Raster<float>({1, 1}, {179}), Raster<std::uint8_t>({1, 1}, {1})},
        0.1, 0.1};
    observations.validate();
    observations.footprint_width_deg = 0;
    rejects([&] { observations.validate(); });
    NativeGeometry image = ImageGeometry{{7,11}};
    validate_geometry(image);
    require(geometry_shape(image) == Shape{7,11});
    rejects([] { ImageGeometry{}.validate(); });
}
