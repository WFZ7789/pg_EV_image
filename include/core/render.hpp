/// SPDX-License-Identifier: MIT

#pragma once

#include "core/execution.hpp"

namespace fy::core {

struct MapPoint {
    double x = 0;
    double y = 0;
    bool valid = false;
};

struct MapProjection {
    bool orthographic = false;
    double center_latitude = 0;
    double center_longitude = 0;
    MapPoint project(double latitude, double longitude) const;
};

struct MapBounds {
    double left = 0;
    double right = 0;
    double bottom = 0;
    double top = 0;
};

struct MapView {
    MapProjection projection;
    MapBounds bounds;
    Shape pixels;
    void validate() const;
};

struct PixelPoint {
    float x = 0;
    float y = 0;
};

struct ProjectedSwath {
    ProfileReport::BufferReservation memory;
    Raster<PixelPoint> vertices;
};

ProjectedSwath project_swath(const SwathGeometry& geometry, const MapView& view, ExecutionContext& context);
ProjectedSwath project_disk(const NativeDiskGeometry& geometry, const MapView& view, ExecutionContext& context);

struct SwathRasterBlock {
    ProfileReport::BufferReservation memory;
    Window window;
    Raster<PixelPoint> vertices;
    Raster<std::uint8_t> valid;
    std::vector<Raster<float>> planes;

    SwathRasterBlock(Window region, std::size_t plane_count, ProfileReport& profile);
    std::size_t bytes() const;
};

/// @note 数值、掩膜、顶点和 mapper 捕获的对象必须存活到栅格化完成；mapper 必须可并发读取。
struct SwathRasterSource {
    Shape cells;
    RasterView<const PixelPoint> vertices;
    std::vector<RasterView<const float>> planes;
    std::vector<RasterView<const std::uint8_t>> valid;
    DisplayDescriptor display;
    std::uint64_t scene_order = 0;
    std::size_t source_column = 0;
    std::size_t source_columns = 0;
    std::function<std::unique_ptr<SwathRasterBlock>(Window)> read_block = {};
    std::size_t block_size = 128;
    std::size_t plane_count = 0;
    double horizontal_period_pixels = 0;
    void validate() const;
};

enum class OverlapPolicy { last_sample, scan_center };

struct RasterOptions {
    LayoutTransform layout;
    OverlapPolicy overlap = OverlapPolicy::last_sample;
    std::size_t tile_size = 64;
    int bit_depth = 8;
    bool keep_coverage = false;
    std::size_t source_cache_bytes = 16 * 1024 * 1024;
};

struct Pixel8 { std::array<std::uint8_t, 3> bgr{}; };
struct Pixel16 { std::array<std::uint16_t, 3> bgr{}; };

struct RasterizedFrame {
    ProfileReport::BufferReservation framebuffer_memory;
    std::optional<ProfileReport::BufferReservation> coverage_memory;
    LayoutTransform layout;
    std::variant<Raster<Pixel8>, Raster<Pixel16>> pixels;
    std::optional<Raster<float>> coverage;

    RasterizedFrame(const RasterOptions& options, ProfileReport& report);
};

RasterizedFrame rasterize_swaths(std::span<const SwathRasterSource> sources,
                                 const RasterOptions& options, ExecutionContext& context);

/// @note 规则网格覆盖整个地图主体；数值与掩膜须存活至绘制结束。
struct ImageRasterSource {
    Shape cells;
    std::vector<RasterView<const float>> planes;
    std::vector<RasterView<const std::uint8_t>> valid;
    DisplayDescriptor display;
    void validate() const;
};

RasterizedFrame rasterize_image(const ImageRasterSource& source, const RasterOptions& options,
                                ExecutionContext& context);

}
