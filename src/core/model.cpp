/// SPDX-License-Identifier: MIT

#include "core/model.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace fy::core {
namespace {

void require_shape(Shape actual, Shape expected) {
    if (actual != expected) throw std::invalid_argument("Plane shape mismatch");
}

void validate_mask(const Raster<std::uint8_t>& mask, Shape expected) {
    require_shape(mask.shape(), expected);
    for (auto value : mask.values())
        if (value > 1) throw std::invalid_argument("Validity must be zero or one");
}

void validate_values(const Raster<float>& values, const Raster<std::uint8_t>& mask) {
    validate_mask(mask, values.shape());
    for (std::size_t i = 0; i < values.values().size(); ++i)
        if (mask.values()[i] && !std::isfinite(values.values()[i]))
            throw std::invalid_argument("Nonfinite valid sample");
}

std::size_t scaled_margin(double margin, double scale) {
    const double result = std::round(margin * scale);
    if (!std::isfinite(margin) || margin < 0 || !std::isfinite(result) ||
        result >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
        throw std::invalid_argument("Invalid layout margin");
    return static_cast<std::size_t>(result);
}

std::size_t add_extent(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a)
        throw std::overflow_error("Layout extent overflow");
    return a + b;
}

}

const GridDescriptor& SensorMetadata::grid(const std::string& id) const {
    auto found = std::ranges::find(grids, id, &GridDescriptor::id);
    if (found == grids.end()) throw std::invalid_argument("Unknown grid: " + id);
    return *found;
}

void SensorMetadata::validate() const {
    if (satellite.empty() || sensor.empty() || resolution.empty() || grids.empty())
        throw std::invalid_argument("Incomplete sensor metadata");
    std::set<std::string> ids;
    for (const auto& item : grids) {
        if (item.id.empty() || !ids.insert(item.id).second || !item.shape.elements())
            throw std::invalid_argument("Invalid or duplicate grid");
        for (std::size_t axis = 0; axis < 2; ++axis)
            if (!std::isfinite(item.index_scale[axis]) || item.index_scale[axis] <= 0 ||
                !std::isfinite(item.index_offset[axis]))
                throw std::invalid_argument("Invalid grid index mapping");
    }
    for (const auto& item : grids)
        if (!item.paired_grid.empty()) grid(item.paired_grid);
}

void SwathGeometry::validate() const {
    if (!latitude.shape().elements()) throw std::invalid_argument("Empty geometry");
    require_shape(longitude.shape(), latitude.shape());
    validate_mask(valid, latitude.shape());
    for (std::size_t i = 0; i < latitude.values().size(); ++i)
        if (valid.values()[i]) {
            const auto lat = latitude.values()[i], lon = longitude.values()[i];
            if (!std::isfinite(lat) || lat < -90 || lat > 90 ||
                !std::isfinite(lon) || lon < -180 || lon > 360)
                throw std::invalid_argument("Invalid geographic coordinate marked valid");
        }
}

SwathGeometry subset_swath_geometry(const SwathGeometry& geometry,Window window) {
    window.validate(geometry.latitude.shape());
    SwathGeometry result{Raster<float>(window.shape),Raster<float>(window.shape),Raster<std::uint8_t>(window.shape),
                         geometry.scan_rows,geometry.first_source_row+window.row};
    for (std::size_t row = 0; row < window.shape.rows; ++row) {
        const auto source = (window.row+row)*geometry.latitude.shape().cols+window.col,target = row*window.shape.cols;
        std::copy_n(geometry.latitude.values().begin()+source,window.shape.cols,result.latitude.values().begin()+target);
        std::copy_n(geometry.longitude.values().begin()+source,window.shape.cols,result.longitude.values().begin()+target);
        std::copy_n(geometry.valid.values().begin()+source,window.shape.cols,result.valid.values().begin()+target);
    }
    return result;
}

void NativeDiskGeometry::validate() const {
    if (!shape.elements() || !std::isfinite(longitude_deg) ||
        !std::isfinite(equatorial_radius_m) || equatorial_radius_m <= 0 ||
        !std::isfinite(polar_radius_m) || polar_radius_m <= 0 || polar_radius_m > equatorial_radius_m ||
        !std::isfinite(satellite_distance_m) || satellite_distance_m <= equatorial_radius_m)
        throw std::invalid_argument("Invalid disk geometry");
    for (std::size_t axis = 0; axis < 2; ++axis)
        if (!std::isfinite(scan_origin_rad[axis]) || !std::isfinite(scan_step_rad[axis]) ||
            scan_step_rad[axis] == 0)
            throw std::invalid_argument("Invalid disk scan coordinates");
}

void ObservationGeometry::validate() const {
    centers.validate();
    if (!std::isfinite(footprint_width_deg) || footprint_width_deg <= 0 ||
        !std::isfinite(footprint_height_deg) || footprint_height_deg <= 0)
        throw std::invalid_argument("Invalid observation footprint");
}

void ImageGeometry::validate() const {
    if (!shape.elements()) throw std::invalid_argument("Empty image geometry");
}

Shape geometry_shape(const NativeGeometry& geometry) {
    return std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, NativeDiskGeometry> || std::is_same_v<T, ImageGeometry>) return item.shape;
        else if constexpr (std::is_same_v<T, ObservationGeometry>) return item.centers.latitude.shape();
        else return item.latitude.shape();
    }, geometry);
}

void validate_geometry(const NativeGeometry& geometry) {
    std::visit([](const auto& item) { item.validate(); }, geometry);
}

void SampleBlock::validate() const {
    if (!window.shape.elements()) throw std::invalid_argument("Empty sample block");
    require_shape(values.shape(), window.shape);
    validate_values(values, valid);
}

void ProductFrame::validate() const {
    if (!geometry || planes.empty() || name.empty()) throw std::invalid_argument("Incomplete frame");
    validate_geometry(*geometry);
    for (const auto& plane : planes) {
        require_shape(plane.values.shape(), geometry_shape(*geometry));
        if (plane.unit.empty()) throw std::invalid_argument("Missing physical unit");
        validate_values(plane.values, plane.valid);
    }
}

void SwathTileView::validate() const {
    if (!source_window.shape.elements() || planes.empty() || planes.size() != value_valid.size())
        throw std::invalid_argument("Incomplete tile view");
    require_shape(latitude.shape(), source_window.shape);
    require_shape(longitude.shape(), source_window.shape);
    require_shape(location_valid.shape(), source_window.shape);
    for (std::size_t i = 0; i < planes.size(); ++i) {
        require_shape(planes[i].shape(), source_window.shape);
        require_shape(value_valid[i].shape(), source_window.shape);
    }
}

void ProductPlan::validate(const SensorMetadata& metadata, std::span<const BandDescriptor> bands) const {
    metadata.validate();
    if (regional_default_half_range && (!std::isfinite(*regional_default_half_range) ||
        *regional_default_half_range <= 0 || *regional_default_half_range > 90))
        throw std::invalid_argument("Invalid default product region");
    if (mode.empty() || frames.empty()) throw std::invalid_argument("Incomplete product plan");
    metadata.grid(calculation_grid);
    std::set<int> band_ids;
    for (const auto& band : bands) {
        if (!band_ids.insert(band.id).second || band.unit.empty() || band.dataset.empty())
            throw std::invalid_argument("Invalid band descriptor");
        metadata.grid(band.grid);
        if (band.valid_range && (!std::isfinite((*band.valid_range)[0]) ||
            !std::isfinite((*band.valid_range)[1]) || (*band.valid_range)[0] > (*band.valid_range)[1]))
            throw std::invalid_argument("Invalid band range");
    }
    for (const auto& dependency : inputs) {
        if (!band_ids.contains(dependency.band)) throw std::invalid_argument("Unknown input band");
        metadata.grid(dependency.target_grid);
    }
    for (const auto& spec : statistics) {
        if (!std::isfinite(spec.percentile) || spec.percentile < 0 || spec.percentile > 100)
            throw std::invalid_argument("Invalid percentile");
        if (!spec.sample_divisor || (spec.upper_cutoff && !std::isfinite(*spec.upper_cutoff)))
            throw std::invalid_argument("Invalid statistics selection");
        if (spec.operation != StatisticsOperation::percentile && spec.rank == RankRule::histogram_ceiling_rank)
            throw std::invalid_argument("Histogram rank requires a percentile");
        if (spec.rank == RankRule::histogram_ceiling_rank && (!spec.histogram_range ||
            spec.histogram_bins < 2 || !std::isfinite((*spec.histogram_range)[0]) ||
            !std::isfinite((*spec.histogram_range)[1]) ||
            (*spec.histogram_range)[0] >= (*spec.histogram_range)[1]))
            throw std::invalid_argument("Invalid histogram statistics");
    }
    std::set<std::string> frame_names;
    for (const auto& frame : frames) {
        if (!frame.grid.empty()) metadata.grid(frame.grid);
        if (frame.name.empty() || !frame_names.insert(frame.name).second || frame.units.empty() ||
            (frame.bit_depth != 8 && frame.bit_depth != 16))
            throw std::invalid_argument("Invalid frame plan");
        for (const auto& unit : frame.units)
            if (unit.empty()) throw std::invalid_argument("Missing frame unit");
    }
}

std::array<double, 2> LayoutTransform::map_anchor(double x, double y) const {
    return {static_cast<double>(left) + x * scale, static_cast<double>(top) + y * scale};
}

LayoutTransform LayoutTransform::make(Shape map, const LayoutDescriptor& descriptor) {
    if (!map.elements() || !std::isfinite(descriptor.baseline_map_width) || descriptor.baseline_map_width <= 0)
        throw std::invalid_argument("Invalid layout dimensions");
    LayoutTransform result;
    result.map = map;
    result.scale = static_cast<double>(map.cols) / descriptor.baseline_map_width;
    result.left = scaled_margin(descriptor.left, result.scale);
    result.top = scaled_margin(descriptor.top, result.scale);
    result.framebuffer = {
        add_extent(add_extent(map.rows, result.top), scaled_margin(descriptor.bottom, result.scale)),
        add_extent(add_extent(map.cols, result.left), scaled_margin(descriptor.right, result.scale))};
    result.framebuffer.elements();
    return result;
}

}
