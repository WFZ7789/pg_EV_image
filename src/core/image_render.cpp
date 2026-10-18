/// SPDX-License-Identifier: MIT

#include "core/render.hpp"
#include "core/navigation.hpp"

#include <algorithm>
#include <cmath>

namespace fy::core {

ProjectedSwath project_disk(const NativeDiskGeometry& geometry,const MapView& view,ExecutionContext& context) {
    geometry.validate(); view.validate();
    const Shape vertices_shape{geometry.shape.rows + 1,geometry.shape.cols + 1};
    if (!vertices_shape.elements() || vertices_shape.elements() > std::numeric_limits<std::size_t>::max() / sizeof(PixelPoint))
        throw std::overflow_error("Disk vertex extent overflow");
    auto memory = context.profile().reserve(BufferKind::scene,vertices_shape.elements() * sizeof(PixelPoint));
    Raster<PixelPoint> vertices(vertices_shape);
    auto phase = context.profile().phase("projection");
    context.parallel_rows(vertices_shape.rows,[&](std::size_t begin,std::size_t end) {
        for (auto row = begin; row < end; ++row)
            for (std::size_t col = 0; col < vertices_shape.cols; ++col) {
                const auto position = disk_geographic_at(geometry,double(row) - 0.5,double(col) - 0.5);
                const auto projected = position.valid ? view.projection.project(position.latitude,position.longitude) : MapPoint{};
                const auto x = (projected.x - view.bounds.left) * view.pixels.cols / (view.bounds.right - view.bounds.left);
                const auto y = (view.bounds.top - projected.y) * view.pixels.rows / (view.bounds.top - view.bounds.bottom);
                vertices.view().at(row,col) = projected.valid ? PixelPoint{float(x),float(y)} : PixelPoint{NAN,NAN};
            }
    });
    return {std::move(memory),std::move(vertices)};
}

void ImageRasterSource::validate() const {
    if (!cells.elements() || planes.empty() || planes.size() != valid.size() || !display.mapper)
        throw std::invalid_argument("Incomplete image raster source");
    for (std::size_t i = 0; i < planes.size(); ++i)
        if (planes[i].shape() != cells || valid[i].shape() != cells)
            throw std::invalid_argument("Image source plane shape mismatch");
}

RasterizedFrame rasterize_image(const ImageRasterSource& source,const RasterOptions& options,ExecutionContext& context) {
    source.validate();
    const auto& layout = options.layout;
    const auto shape = layout.map;
    if (!options.tile_size || (options.bit_depth != 8 && options.bit_depth != 16) ||
        !shape.elements() || !layout.framebuffer.elements() || layout.left > layout.framebuffer.cols ||
        layout.top > layout.framebuffer.rows || shape.cols > layout.framebuffer.cols - layout.left ||
        shape.rows > layout.framebuffer.rows - layout.top)
        throw std::invalid_argument("Invalid image rasterization layout");
    RasterizedFrame result(options,context.profile());
    auto phase = context.profile().phase("image_rasterization");
    const double scale_x = double(source.cells.cols) / shape.cols, scale_y = double(source.cells.rows) / shape.rows;
    context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
        auto memory = context.profile().reserve(BufferKind::worker_tiles,source.planes.size() * sizeof(float));
        std::vector<float> values(source.planes.size());
        for (auto row = begin; row < end; ++row) {
            const double top = row * scale_y, bottom = std::min(double(source.cells.rows),(row + 1) * scale_y);
            for (std::size_t col = 0; col < shape.cols; ++col) {
                const double left = col * scale_x, right = std::min(double(source.cells.cols),(col + 1) * scale_x);
                Color sum{0,0,0,0};
                double coverage = 0;
                for (auto y = static_cast<std::size_t>(std::floor(top)); y < std::min(source.cells.rows,static_cast<std::size_t>(std::ceil(bottom))); ++y)
                    for (auto x = static_cast<std::size_t>(std::floor(left)); x < std::min(source.cells.cols,static_cast<std::size_t>(std::ceil(right))); ++x) {
                        bool valid = true;
                        for (std::size_t channel = 0; channel < values.size(); ++channel) {
                            values[channel] = source.planes[channel].at(y,x);
                            valid = valid && source.valid[channel].at(y,x) && std::isfinite(values[channel]);
                        }
                        if (!valid) continue;
                        auto color = source.display.mapper(values,source.display.scale);
                        if (!std::isfinite(color.red) || !std::isfinite(color.green) || !std::isfinite(color.blue) ||
                            !std::isfinite(color.alpha) || color.alpha <= 0) continue;
                        const double area = (std::min(bottom,double(y + 1)) - std::max(top,double(y))) *
                                            (std::min(right,double(x + 1)) - std::max(left,double(x)));
                        const double weight = area / (scale_x * scale_y) * std::clamp(color.alpha,0.0,1.0);
                        sum.red += std::clamp(color.red,0.0,1.0) * weight;
                        sum.green += std::clamp(color.green,0.0,1.0) * weight;
                        sum.blue += std::clamp(color.blue,0.0,1.0) * weight;
                        coverage += area / (scale_x * scale_y);
                    }
                std::visit([&](auto& image) {
                    using Component = std::remove_reference_t<decltype(image.view().at(0,0).bgr[0])>;
                    constexpr auto maximum = std::numeric_limits<Component>::max();
                    image.view().at(layout.top + row,layout.left + col).bgr = {
                        static_cast<Component>(std::lround(std::clamp(sum.blue,0.0,1.0) * maximum)),
                        static_cast<Component>(std::lround(std::clamp(sum.green,0.0,1.0) * maximum)),
                        static_cast<Component>(std::lround(std::clamp(sum.red,0.0,1.0) * maximum))};
                },result.pixels);
                if (result.coverage) result.coverage->view().at(row,col) = float(std::clamp(coverage,0.0,1.0));
            }
        }
    },options.tile_size);
    context.profile().set_output(layout.framebuffer);
    return result;
}

}
