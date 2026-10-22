/// SPDX-License-Identifier: MIT

#pragma once

#include "core/raster.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace fy::core {

enum class SourceRole { data, geolocation, auxiliary };
enum class ChannelAxis { none, first, middle, last };
enum class CalibrationKind { identity, linear, polynomial, lookup, physical };

struct SourceFiles {
    std::filesystem::path data;
    std::filesystem::path geolocation;
    std::filesystem::path auxiliary;
};

struct GridDescriptor {
    std::string id;
    Shape shape;
    SourceRole source = SourceRole::data;
    std::string paired_grid;
    std::array<double, 2> index_scale{1, 1};
    std::array<double, 2> index_offset{0, 0};
};

struct CalibrationDescriptor {
    CalibrationKind kind = CalibrationKind::identity;
    SourceRole source = SourceRole::data;
    std::string provenance;
    std::vector<double> coefficients;
    std::vector<double> fill_values = {};
};

struct BandDescriptor {
    int id = 0;
    std::string name;
    std::string dataset;
    std::string grid;
    std::string unit;
    SourceRole source = SourceRole::data;
    ChannelAxis channel_axis = ChannelAxis::none;
    std::size_t channel_index = 0;
    std::vector<double> fill_values;
    std::optional<std::array<double, 2>> valid_range;
    CalibrationDescriptor calibration;
    std::string calibrated_unit;
};

struct SensorMetadata {
    std::string satellite;
    std::string sensor;
    std::string resolution;
    std::string observation_begin;
    std::string observation_end;
    std::vector<std::string> labels;
    std::vector<GridDescriptor> grids;
    std::map<std::string, double> numeric_attributes;

    const GridDescriptor& grid(const std::string& id) const;
    void validate() const;
};

struct SwathGeometry {
    Raster<float> latitude;
    Raster<float> longitude;
    Raster<std::uint8_t> valid;
    void validate() const;
};

/// @note 扫描角按 [东西, 南北] 排列，步长分别沿列、行增加。
struct NativeDiskGeometry {
    Shape shape;
    double longitude_deg = 0;
    double equatorial_radius_m = 6378137;
    double polar_radius_m = 6356752.3;
    double satellite_distance_m = 42164000;
    std::array<double, 2> scan_origin_rad{0, 0};
    std::array<double, 2> scan_step_rad{0, 0};
    void validate() const;
};

struct ObservationGeometry {
    SwathGeometry centers;
    double footprint_width_deg = 0;
    double footprint_height_deg = 0;
    void validate() const;
};

/// @brief 仅有原生画幅坐标的规则图像网格，不隐含地理定位。
struct ImageGeometry {
    Shape shape;
    void validate() const;
};

using NativeGeometry = std::variant<SwathGeometry, NativeDiskGeometry, ObservationGeometry, ImageGeometry>;
Shape geometry_shape(const NativeGeometry& geometry);
void validate_geometry(const NativeGeometry& geometry);

/// @note retain_finite 仅供既有统计口径使用，不恢复数值有效性。
enum class InvalidSamplePolicy { nan, retain_finite };

struct SampleBlock {
    int band = 0;
    Window window;
    Raster<float> values;
    Raster<std::uint8_t> valid;
    void validate() const;
};

struct ScalarPlane {
    std::string unit;
    Raster<float> values;
    Raster<std::uint8_t> valid;
};

struct ProductFrame {
    std::string name;
    std::vector<ScalarPlane> planes;
    std::shared_ptr<const NativeGeometry> geometry;
    void validate() const;
};

struct SwathTileView {
    Window source_window;
    RasterView<const float> latitude;
    RasterView<const float> longitude;
    RasterView<const std::uint8_t> location_valid;
    std::vector<RasterView<const float>> planes;
    std::vector<RasterView<const std::uint8_t>> value_valid;
    void validate() const;
};

enum class StatisticsDomain { scene, task, frame_sequence };
enum class RankRule { legacy_floor_index, histogram_ceiling_rank };
enum class StatisticsOperation { percentile, minmax, mean, top_mean };

struct StatisticsSpec {
    StatisticsDomain domain = StatisticsDomain::scene;
    RankRule rank = RankRule::legacy_floor_index;
    bool before_crop = true;
    bool exclude_nan_only = true;
    double percentile = 99;
    std::optional<std::array<double, 2>> histogram_range;
    std::size_t histogram_bins = 0;
    StatisticsOperation operation = StatisticsOperation::percentile;
    std::string field;
    std::size_t sample_divisor = 1;
    std::optional<double> upper_cutoff;
    bool ordered = false;
    bool include_masked_finite = false;
};

struct InputDependency {
    int band = 0;
    std::string target_grid;
    std::size_t halo_rows = 0;
    std::size_t halo_cols = 0;
    bool complete_row_prefix = false;
    bool complete_scene_parameters = false;
    /// @brief 显式使用原始样本，仍执行有效性检查。
    bool raw_samples = false;
    /// @brief 以 360 度周期插值方位角。
    bool cyclic_degrees = false;
    /// @brief 使用观测经纬度对齐，不能以数组尺寸推断共址。
    bool geographic_alignment = false;
};

struct FramePlan {
    std::string name;
    std::vector<std::string> units;
    int bit_depth = 8;
    std::optional<std::size_t> spectral_channel;
    std::string grid = {};
};

struct ProductPlan {
    std::string mode;
    std::string calculation_grid;
    std::vector<InputDependency> inputs;
    std::vector<StatisticsSpec> statistics;
    std::vector<FramePlan> frames;
    bool stream_frames = false;
    std::optional<double> regional_default_half_range;
    /// @brief 在区域读取包围盒内继续屏蔽经纬度范围之外的观测。
    bool clip_to_region = false;
    void validate(const SensorMetadata& metadata, std::span<const BandDescriptor> bands) const;
};

struct DisplayScale {
    double low = 0;
    double high = 1;
    std::array<double,3> rgb_gain{1,1,1};
};

struct Color {
    double red = 0;
    double green = 0;
    double blue = 0;
    double alpha = 1;
};

enum class ColorSpace { display_encoded, linear };

struct DisplayDescriptor {
    DisplayScale scale;
    ColorSpace color_space = ColorSpace::display_encoded;
    bool colorbar = false;
    std::function<Color(std::span<const float>, const DisplayScale&)> mapper;
    bool balanced_rgb = false;
};

struct LayoutDescriptor {
    double baseline_map_width = 3840;
    double left = 0;
    double top = 162;
    double right = 300;
    double bottom = 162;
};

struct LayoutTransform {
    Shape map;
    Shape framebuffer;
    std::size_t left = 0;
    std::size_t top = 0;
    double scale = 1;
    std::array<double, 2> map_anchor(double x, double y) const;
    static LayoutTransform make(Shape map, const LayoutDescriptor& descriptor = {});
};

}
