/// SPDX-License-Identifier: MIT

#include "products/execute.hpp"
#include "core/resample.hpp"
#include "core/stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fy::products {
namespace {

using namespace core;

std::uint64_t storage_bytes(Shape shape,std::size_t bytes_per_pixel) {
    const auto size = shape.elements();
    if (size > std::numeric_limits<std::uint64_t>::max() / bytes_per_pixel)
        throw std::overflow_error("Product storage size overflow");
    return size * bytes_per_pixel;
}

std::uint64_t geometry_bytes(const NativeGeometry& geometry) {
    return std::visit([](const auto& value) -> std::uint64_t {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,SwathGeometry>)
            return value.latitude.bytes() + value.longitude.bytes() + value.valid.bytes();
        else if constexpr (std::is_same_v<T,ObservationGeometry>)
            return value.centers.latitude.bytes() + value.centers.longitude.bytes() + value.centers.valid.bytes();
        else return sizeof(T);
    },geometry);
}

ScalarPlane load_plane(SensorSession& session,const BandDescriptor& band,Window window,ExecutionContext& context,
                       InvalidSamplePolicy invalid,bool raw_samples) {
    const auto shape = window.shape;
    ScalarPlane plane{band.calibrated_unit.empty() ? band.unit : band.calibrated_unit,Raster<float>(shape),Raster<std::uint8_t>(shape)};
    stream_calibrated(session,band.id,window,context,[&](SampleTile&& tile) {
        const auto offset = (tile.samples.window.row - window.row) * shape.cols;
        std::memcpy(plane.values.values().data() + offset,tile.samples.values.values().data(),tile.samples.values.bytes());
        std::memcpy(plane.valid.values().data() + offset,tile.samples.valid.values().data(),tile.samples.valid.bytes());
    },0,invalid,raw_samples ? std::optional(CalibrationDescriptor{}) : std::nullopt);
    return plane;
}

Window select_region(const SwathGeometry& geometry,const GeographicRegion& region) {
    if (!std::isfinite(region.latitude) || std::abs(region.latitude) > 90 || !std::isfinite(region.longitude) ||
        !std::isfinite(region.half_range) || region.half_range <= 0 || region.half_range > 90)
        throw std::invalid_argument("Invalid geographic crop");
    const auto shape = geometry.latitude.shape();
    std::size_t first_row = shape.rows, first_col = shape.cols, last_row = 0, last_col = 0;
    for (std::size_t row = 0; row < shape.rows; ++row)
        for (std::size_t col = 0; col < shape.cols; ++col) {
            const auto i = row * shape.cols + col;
            if (geometry.valid.values()[i] && std::abs(geometry.latitude.values()[i] - region.latitude) <= region.half_range &&
                std::abs(std::remainder(geometry.longitude.values()[i] - region.longitude,360.0)) <= region.half_range) {
                first_row = std::min(first_row,row); first_col = std::min(first_col,col);
                last_row = std::max(last_row,row); last_col = std::max(last_col,col);
            }
        }
    if (first_row == shape.rows) throw std::invalid_argument("Requested region is outside the input coverage");
    return {first_row,first_col,{last_row - first_row + 1,last_col - first_col + 1}};
}

template<class T>
Raster<T> copy_window(const Raster<T>& source,Window window) {
    Raster<T> result(window.shape);
    for (std::size_t row = 0; row < window.shape.rows; ++row)
        std::copy_n(source.values().begin() + (window.row + row) * source.shape().cols + window.col,
                    window.shape.cols,result.values().begin() + row * window.shape.cols);
    return result;
}

}

LoadedProduct load_product(SensorSession& session,const ProductDefinition& definition,ExecutionContext& context,
                                 std::optional<GeographicRegion> region) {
    const auto metadata = session.metadata();
    const auto bands = session.bands();
    definition.plan.validate(metadata,bands);
    if (definition.plan.frames.size() != 1 || !definition.prepare) throw std::invalid_argument("Invalid product executor");
    const auto& frame = definition.plan.frames.front();
    const auto& target_grid = metadata.grid(frame.grid.empty() ? definition.plan.calculation_grid : frame.grid);
    const auto source_grid = target_grid.paired_grid.empty() ? target_grid.id : target_grid.paired_grid;
    const auto source_shape = metadata.grid(source_grid).shape, full_shape = target_grid.shape;
    auto shape = full_shape;
    Window target_window{0,0,shape};
    auto memory = context.profile().reserve(BufferKind::scene,0);
    std::shared_ptr<const NativeGeometry> geometry;
    {
        auto source = session.read_geometry(source_grid,{0,0,source_shape});
        auto source_memory = context.profile().reserve(BufferKind::scene,geometry_bytes(source));
        if (source_shape == shape) {
            geometry = std::make_shared<NativeGeometry>(std::move(source));
            memory = std::move(source_memory);
        } else {
            auto target_memory = context.profile().reserve(BufferKind::scene,storage_bytes(shape,9));
            auto workspace = context.profile().reserve(BufferKind::statistics,storage_bytes(shape,1));
            auto result = resample_geolocation(std::get<SwathGeometry>(source),{0,0,source_shape},source_shape,{0,0,shape},shape,context);
            geometry = std::make_shared<NativeGeometry>(std::move(result));
            memory = std::move(target_memory);
        }
    }
    if (region) {
        if (std::ranges::any_of(definition.plan.statistics,&StatisticsSpec::before_crop) ||
            std::ranges::any_of(definition.plan.inputs,[](const auto& input) {
                return input.halo_rows || input.halo_cols || input.complete_row_prefix || input.complete_scene_parameters;
            })) throw std::invalid_argument("Product dependencies require the complete scene before crop");
        const auto& swath = std::get<SwathGeometry>(*geometry);
        target_window = select_region(swath,*region);
        shape = target_window.shape;
        auto cropped_memory = context.profile().reserve(BufferKind::scene,storage_bytes(shape,9));
        geometry = std::make_shared<NativeGeometry>(SwathGeometry{copy_window(swath.latitude,target_window),
            copy_window(swath.longitude,target_window),copy_window(swath.valid,target_window)});
        memory = std::move(cropped_memory);
    }
    std::uint64_t owned_bytes = geometry_bytes(*geometry);
    const auto invalid = std::ranges::any_of(definition.plan.statistics, &StatisticsSpec::include_masked_finite) ?
                         InvalidSamplePolicy::retain_finite : InvalidSamplePolicy::nan;
    std::vector<ScalarPlane> inputs;
    for (const auto& dependency : definition.plan.inputs) {
        const auto found = std::ranges::find(bands,dependency.band,&BandDescriptor::id);
        const auto band_shape = metadata.grid(found->grid).shape;
        const auto input_shape = metadata.grid(dependency.target_grid).shape;
        if (region && dependency.target_grid != target_grid.id)
            throw std::invalid_argument("Multiple calculation grids require the complete scene before crop");
        const Window input_target = dependency.target_grid == target_grid.id ? target_window : Window{0,0,input_shape};
        const auto bytes = storage_bytes(input_target.shape,sizeof(float) + sizeof(std::uint8_t));
        if (bytes > std::numeric_limits<std::uint64_t>::max() - owned_bytes)
            throw std::overflow_error("Product input storage overflow");
        owned_bytes += bytes;
        memory.resize(owned_bytes);
        if (band_shape == input_shape) inputs.push_back(load_plane(session,*found,input_target,context,invalid,dependency.raw_samples));
        else {
            const auto input_window = cubic_source_window(band_shape,input_shape,input_target);
            auto source_memory = context.profile().reserve(BufferKind::scene,storage_bytes(input_window.shape,5));
            auto source = load_plane(session,*found,input_window,context,invalid,dependency.raw_samples);
            auto plane = resample_cubic(source.values.view(),source.valid.view(),input_window,band_shape,
                                        input_target,input_shape,dependency.cyclic_degrees,context,invalid);
            plane.unit = found->calibrated_unit;
            inputs.push_back(std::move(plane));
        }
    }
    const auto output_planes = definition.plan.frames.front().units.size();
    if (output_planes > inputs.size()) memory.resize(owned_bytes + storage_bytes(shape,(output_planes - inputs.size()) * 5));
    auto product = definition.prepare(std::move(inputs),geometry,context);
    if (product.frame.planes.size() != output_planes) throw std::invalid_argument("Product output plane count differs from its plan");
    std::uint64_t result_bytes = geometry_bytes(*geometry);
    for (const auto& plane : product.frame.planes) result_bytes += plane.values.bytes() + plane.valid.bytes();
    memory.resize(result_bytes);
    context.profile().set_source_pixels(shape.elements());
    return {std::move(memory),std::move(product),target_window,full_shape};
}

}
