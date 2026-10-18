/// SPDX-License-Identifier: MIT

#include "core/render.hpp"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <memory_resource>
#include <numbers>
#include <unordered_map>

namespace fy::core {
namespace {

constexpr double radians = std::numbers::pi / 180.0;

using Quad = std::array<PixelPoint, 4>;

std::size_t block_bytes(Shape shape,std::size_t planes) {
    const auto count = shape.elements(),vertices = Shape{shape.rows + 1,shape.cols + 1}.elements();
    if (!count || !planes || vertices > std::numeric_limits<std::size_t>::max() / sizeof(PixelPoint) ||
        planes > (std::numeric_limits<std::size_t>::max() - 1) / sizeof(float))
        throw std::overflow_error("Raster block extent overflow");
    const auto vertex_bytes = vertices * sizeof(PixelPoint), stride = planes * sizeof(float) + 1;
    if (count > (std::numeric_limits<std::size_t>::max() - vertex_bytes) / stride)
        throw std::overflow_error("Raster block allocation overflow");
    return vertex_bytes + count * stride;
}

class SourceReader {
    struct Key {
        const SwathRasterSource* source;
        std::size_t row,col;
        bool operator==(const Key&) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key& key) const {
            return std::hash<const void*>{}(key.source) ^ (std::hash<std::size_t>{}(key.row) << 1) ^
                   (std::hash<std::size_t>{}(key.col) << 2);
        }
    };
    struct Entry {
        Key key;
        std::unique_ptr<SwathRasterBlock> block;
        SwathRasterSource view;
    };
    using List = std::list<Entry>;
    List cache_;
    std::unordered_map<Key,List::iterator,Hash> lookup_;
    Entry* last_ = nullptr;
    std::size_t bytes_ = 0,budget_;
public:
    struct Cell { const SwathRasterSource* source; std::size_t index; };
    explicit SourceReader(std::size_t budget) : budget_(budget) {}

    Cell cell(const SwathRasterSource& source,std::size_t index) {
        if (!source.read_block) return {&source,index};
        const auto row = index / source.cells.cols,col = index % source.cells.cols;
        const Key key{&source,row / source.block_size * source.block_size,col / source.block_size * source.block_size};
        if (!last_ || last_->key != key) {
            auto found = lookup_.find(key);
            if (found != lookup_.end()) cache_.splice(cache_.begin(),cache_,found->second);
            else {
                const Window window{key.row,key.col,{std::min(source.block_size,source.cells.rows - key.row),
                                                     std::min(source.block_size,source.cells.cols - key.col)}};
                const auto bytes = block_bytes(window.shape,source.plane_count);
                if (bytes > budget_) throw std::invalid_argument("Source cache budget is smaller than one block");
                while (bytes_ > budget_ - bytes) {
                    bytes_ -= cache_.back().block->bytes();
                    lookup_.erase(cache_.back().key); cache_.pop_back();
                }
                last_ = nullptr;
                auto block = source.read_block(window);
                if (!block || block->window.row != window.row || block->window.col != window.col ||
                    block->window.shape != window.shape || block->planes.size() != source.plane_count ||
                    block->vertices.shape() != Shape{window.shape.rows + 1,window.shape.cols + 1} || block->valid.shape() != window.shape)
                    throw std::invalid_argument("Source provider returned an invalid block");
                SwathRasterSource view;
                view.cells = window.shape; view.vertices = block->vertices.view(); view.display = source.display;
                view.horizontal_period_pixels = source.horizontal_period_pixels;
                for (const auto& plane : block->planes) {
                    view.planes.push_back(plane.view()); view.valid.push_back(block->valid.view());
                }
                view.validate();
                cache_.push_front({key,std::move(block),std::move(view)});
                lookup_.emplace(key,cache_.begin()); bytes_ += bytes;
            }
            last_ = &cache_.front();
        }
        return {&last_->view,(row - key.row) * last_->view.cells.cols + col - key.col};
    }
};

struct Point {
    double x, y;
};

using Polygon = std::pmr::vector<Point>;

double cross(Point a, Point b, Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

double signed_area(const Quad& quad) {
    double result = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& a = quad[i];
        const auto& b = quad[(i + 1) % 4];
        result += double(a.x) * b.y - double(a.y) * b.x;
    }
    return result * 0.5;
}

double area(const Polygon& polygon) {
    if (polygon.size() < 3) return 0;
    double result = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % polygon.size()];
        result += a.x * b.y - a.y * b.x;
    }
    return std::abs(result) * 0.5;
}

Polygon clip(const Polygon& polygon, Point a, Point b, double orientation, bool inside,
             std::pmr::memory_resource* memory) {
    Polygon result(memory);
    if (polygon.empty()) return result;
    result.reserve(polygon.size() + 1);
    auto previous = polygon.back();
    auto previous_distance = orientation * cross(a, b, previous);
    bool previous_kept = inside ? previous_distance >= 0 : previous_distance <= 0;
    for (const auto& current : polygon) {
        const auto distance = orientation * cross(a, b, current);
        const bool kept = inside ? distance >= 0 : distance <= 0;
        if (kept != previous_kept) {
            const auto t = previous_distance / (previous_distance - distance);
            result.push_back({previous.x + t * (current.x - previous.x),
                              previous.y + t * (current.y - previous.y)});
        }
        if (kept) result.push_back(current);
        previous = current;
        previous_distance = distance;
        previous_kept = kept;
    }
    return result;
}

class TrackedMemory final : public std::pmr::memory_resource {
public:
    explicit TrackedMemory(ProfileReport& report) : report_(report) {}
private:
    ProfileReport& report_;
    std::map<void*, ProfileReport::BufferReservation> allocations_;

    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto reservation = report_.reserve(BufferKind::worker_tiles, bytes);
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        try { allocations_.emplace(result, std::move(reservation)); }
        catch (...) {
            std::pmr::new_delete_resource()->deallocate(result, bytes, alignment);
            throw;
        }
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        allocations_.erase(pointer);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

struct Fragment {
    Polygon polygon;
    double left = INFINITY, right = -INFINITY, top = INFINITY, bottom = -INFINITY;

    explicit Fragment(Polygon&& points) : polygon(std::move(points)) {
        for (auto point : polygon) {
            left = std::min(left,point.x); right = std::max(right,point.x);
            top = std::min(top,point.y); bottom = std::max(bottom,point.y);
        }
    }
};

struct PixelWork {
    Color sum{0,0,0,0};
    double coverage = 0;
    bool initialized = false;
    std::pmr::vector<Fragment> free;
    explicit PixelWork(std::pmr::memory_resource* memory) : free(memory) {}
};

struct Extent {
    std::size_t left, right, top, bottom;
};

Quad quad_at(const SwathRasterSource& source, std::size_t index) {
    const auto row = index / source.cells.cols, col = index % source.cells.cols;
    return {source.vertices.at(row, col), source.vertices.at(row, col + 1),
            source.vertices.at(row + 1, col + 1), source.vertices.at(row + 1, col)};
}

std::optional<Extent> extent(const SwathRasterSource& source, std::size_t index, Shape output) {
    const auto row = index / source.cells.cols, col = index % source.cells.cols;
    for (const auto& valid : source.valid) if (!valid.at(row, col)) return {};
    const auto quad = quad_at(source, index);
    for (const auto& point : quad) if (!std::isfinite(point.x) || !std::isfinite(point.y)) return {};
    if (source.horizontal_period_pixels > 0)
        for (std::size_t i = 0; i < quad.size(); ++i)
            if (std::abs(double(quad[i].x) - quad[(i + 1) % quad.size()].x) > source.horizontal_period_pixels / 2) return {};
    const auto orientation = signed_area(quad);
    if (std::abs(orientation) <= 1e-12) return {};
    for (std::size_t i = 0; i < 4; ++i) {
        auto a = quad[i], b = quad[(i + 1) % 4], c = quad[(i + 2) % 4];
        if (orientation * cross({a.x,a.y}, {b.x,b.y}, {c.x,c.y}) < 0) return {};
    }
    const double left = std::min({quad[0].x,quad[1].x,quad[2].x,quad[3].x});
    const double right = std::max({quad[0].x,quad[1].x,quad[2].x,quad[3].x});
    const double top = std::min({quad[0].y,quad[1].y,quad[2].y,quad[3].y});
    const double bottom = std::max({quad[0].y,quad[1].y,quad[2].y,quad[3].y});
    if (left >= double(output.cols) || top >= double(output.rows) || right <= 0 || bottom <= 0) return {};
    const auto x0 = static_cast<std::size_t>(std::floor(std::max(0.0, left)));
    const auto y0 = static_cast<std::size_t>(std::floor(std::max(0.0, top)));
    const auto x1 = static_cast<std::size_t>(std::ceil(std::min(double(output.cols), right)));
    const auto y1 = static_cast<std::size_t>(std::ceil(std::min(double(output.rows), bottom)));
    if (x0 >= x1 || y0 >= y1) return {};
    return Extent{x0,x1,y0,y1};
}

void contribute(PixelWork& pixel, const Quad& quad, std::size_t x, std::size_t y, Color color,
                std::pmr::memory_resource* memory) {
    if (pixel.initialized && pixel.free.empty()) return;
    std::array<Point,4> local;
    for (std::size_t i = 0; i < 4; ++i) local[i] = {double(quad[i].x) - x, double(quad[i].y) - y};
    const double orientation = signed_area(quad) > 0 ? 1 : -1;
    auto accumulate = [&](double covered) {
        pixel.coverage += covered;
        pixel.sum.red += covered * color.alpha * color.red;
        pixel.sum.green += covered * color.alpha * color.green;
        pixel.sum.blue += covered * color.alpha * color.blue;
    };
    bool full = true;
    for (std::size_t edge = 0; edge < 4 && full; ++edge) {
        const auto a = local[edge],b = local[(edge + 1) % 4];
        const double dx = (b.x - a.x) * orientation,dy = (a.y - b.y) * orientation;
        full = dx * -a.y + dy * -a.x + std::min(0.0,dx) + std::min(0.0,dy) >= 0;
    }
    if (full) {
        accumulate(std::max(0.0,1 - pixel.coverage));
        pixel.initialized = true; pixel.free.clear();
        return;
    }
    if (!pixel.initialized) {
        Polygon square(memory);
        square.assign({{0,0},{1,0},{1,1},{0,1}});
        pixel.free.emplace_back(std::move(square));
        pixel.initialized = true;
    }
    const double left = std::min({local[0].x,local[1].x,local[2].x,local[3].x});
    const double right = std::max({local[0].x,local[1].x,local[2].x,local[3].x});
    const double top = std::min({local[0].y,local[1].y,local[2].y,local[3].y});
    const double bottom = std::max({local[0].y,local[1].y,local[2].y,local[3].y});
    std::pmr::vector<Fragment> remaining(memory);
    double covered = 0;
    for (std::size_t index = 0; index < pixel.free.size();) {
        auto& fragment = pixel.free[index];
        bool disjoint = fragment.left >= right || fragment.right <= left || fragment.top >= bottom || fragment.bottom <= top;
        for (std::size_t edge = 0; edge < 4 && !disjoint; ++edge)
            disjoint = std::ranges::all_of(fragment.polygon,[&](Point point) {
                return orientation * cross(local[edge],local[(edge + 1) % 4],point) <= 0;
            });
        if (disjoint) { ++index; continue; }
        Polygon inside = std::move(fragment.polygon);
        if (index + 1 != pixel.free.size()) fragment = std::move(pixel.free.back());
        pixel.free.pop_back();
        for (std::size_t edge = 0; edge < 4 && !inside.empty(); ++edge) {
            const auto a = local[edge], b = local[(edge + 1) % 4];
            if (std::ranges::all_of(inside,[&](Point point) { return orientation * cross(a,b,point) >= 0; })) continue;
            if (std::ranges::all_of(inside,[&](Point point) { return orientation * cross(a,b,point) <= 0; })) {
                if (area(inside) > 1e-12) remaining.emplace_back(std::move(inside));
                inside.clear();
                break;
            }
            auto outside = clip(inside, a, b, orientation, false, memory);
            if (area(outside) > 1e-12) remaining.emplace_back(std::move(outside));
            inside = clip(inside, a, b, orientation, true, memory);
        }
        covered += area(inside);
    }
    for (auto& fragment : remaining) pixel.free.push_back(std::move(fragment));
    accumulate(covered);
}

std::uint64_t reference_key(const SwathRasterSource& source, std::size_t index, std::uint32_t global,
                            OverlapPolicy policy) {
    if (policy == OverlapPolicy::last_sample) return global;
    const auto columns = source.source_columns ? source.source_columns : source.cells.cols;
    const auto col = source.source_column + index % source.cells.cols;
    const auto score = std::min(col, columns - 1 - col);
    return (std::uint64_t(score) << 32) | (std::numeric_limits<std::uint32_t>::max() - global);
}

std::uint32_t reference_index(std::uint64_t key, OverlapPolicy policy) {
    const auto index = static_cast<std::uint32_t>(key);
    return policy == OverlapPolicy::last_sample ? index : std::numeric_limits<std::uint32_t>::max() - index;
}

}

MapPoint MapProjection::project(double latitude, double longitude) const {
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90 || latitude > 90 ||
        longitude < -180 || longitude > 360 || !std::isfinite(center_latitude) ||
        !std::isfinite(center_longitude) || std::abs(center_latitude) > 90) return {};
    if (!orthographic)
        return {center_longitude + std::remainder(longitude - center_longitude, 360.0), latitude, true};
    const auto lat = latitude * radians, center = center_latitude * radians;
    const auto delta = std::remainder(longitude - center_longitude, 360.0) * radians;
    const auto visibility = std::sin(center) * std::sin(lat) + std::cos(center) * std::cos(lat) * std::cos(delta);
    if (visibility < 0) return {};
    return {std::cos(lat) * std::sin(delta),
            std::cos(center) * std::sin(lat) - std::sin(center) * std::cos(lat) * std::cos(delta), true};
}

void MapView::validate() const {
    if (!pixels.elements() || !std::isfinite(bounds.left) || !std::isfinite(bounds.right) ||
        !std::isfinite(bounds.bottom) || !std::isfinite(bounds.top) ||
        bounds.right <= bounds.left || bounds.top <= bounds.bottom ||
        !std::isfinite(projection.center_latitude) || !std::isfinite(projection.center_longitude) ||
        std::abs(projection.center_latitude) > 90)
        throw std::invalid_argument("Invalid map view");
}

ProjectedSwath project_swath(const SwathGeometry& geometry, const MapView& view, ExecutionContext& context) {
    geometry.validate(); view.validate();
    const auto shape = geometry.latitude.shape();
    const Shape vertices_shape{shape.rows + 1, shape.cols + 1};
    if (vertices_shape.elements() > std::numeric_limits<std::uint64_t>::max() / sizeof(PixelPoint))
        throw std::overflow_error("Projected grid size overflow");
    auto memory = context.profile().reserve(BufferKind::scene, vertices_shape.elements() * sizeof(PixelPoint));
    Raster<PixelPoint> vertices(vertices_shape);
    auto phase = context.profile().phase("projection");
    context.parallel_rows(vertices_shape.rows, [&](std::size_t begin, std::size_t end) {
        auto row_memory = context.profile().reserve(BufferKind::worker_tiles, shape.cols * sizeof(MapPoint) * 2);
        std::vector<MapPoint> upper(shape.cols), lower(shape.cols);
        auto project_row = [&](std::size_t row, std::vector<MapPoint>& points) {
            for (std::size_t col = 0; col < shape.cols; ++col) {
                const auto index = row * shape.cols + col;
                points[col] = geometry.valid.values()[index] ?
                    view.projection.project(geometry.latitude.values()[index], geometry.longitude.values()[index]) : MapPoint{};
            }
        };
        project_row(begin ? begin - 1 : 0, upper);
        project_row(std::min(begin, shape.rows - 1), lower);
        for (auto row = begin; row < end; ++row) {
            for (std::size_t col = 0; col <= shape.cols; ++col) {
                const auto left = col ? col - 1 : 0, right = std::min(col, shape.cols - 1);
                const std::array<MapPoint,4> points{upper[left],upper[right],lower[left],lower[right]};
                double x = 0, y = 0;
                double min_x = std::numeric_limits<double>::infinity(), max_x = -min_x;
                bool valid = true;
                for (const auto& point : points) {
                    valid = valid && point.valid;
                    x += point.x; y += point.y;
                    min_x = std::min(min_x, point.x); max_x = std::max(max_x, point.x);
                }
                if (!view.projection.orthographic && max_x - min_x > 180) valid = false;
                const double pixel_x = (x / 4 - view.bounds.left) * view.pixels.cols / (view.bounds.right - view.bounds.left);
                const double pixel_y = (view.bounds.top - y / 4) * view.pixels.rows / (view.bounds.top - view.bounds.bottom);
                vertices.view().at(row,col) = valid ? PixelPoint{float(pixel_x),float(pixel_y)} : PixelPoint{NAN,NAN};
            }
            upper.swap(lower);
            if (row + 1 < end) project_row(std::min(row + 1, shape.rows - 1), lower);
        }
    });
    return {std::move(memory), std::move(vertices)};
}

void SwathRasterSource::validate() const {
    if (!cells.elements() || !display.mapper)
        throw std::invalid_argument("Incomplete raster source");
    if (!std::isfinite(horizontal_period_pixels) || horizontal_period_pixels < 0)
        throw std::invalid_argument("Invalid horizontal coordinate period");
    if (read_block) {
        if (!block_size || !plane_count || vertices.shape().elements() || !planes.empty() || !valid.empty())
            throw std::invalid_argument("Invalid cached raster source");
    } else if (vertices.shape() != Shape{cells.rows + 1,cells.cols + 1} || planes.empty() || planes.size() != valid.size())
        throw std::invalid_argument("Incomplete raster source views");
    for (std::size_t i = 0; i < planes.size(); ++i)
        if (planes[i].shape() != cells || valid[i].shape() != cells)
            throw std::invalid_argument("Raster source plane shape mismatch");
    const auto columns = source_columns ? source_columns : cells.cols;
    if (columns > std::numeric_limits<std::uint32_t>::max() ||
        source_column > columns || cells.cols > columns - source_column)
        throw std::invalid_argument("Raster source column extent mismatch");
}

SwathRasterBlock::SwathRasterBlock(Window region,std::size_t plane_count,ProfileReport& profile)
    : memory(profile.reserve(BufferKind::worker_tiles,block_bytes(region.shape,plane_count))),window(region),
      vertices({region.shape.rows + 1,region.shape.cols + 1}),valid(region.shape) {
    planes.reserve(plane_count);
    for (std::size_t i = 0; i < plane_count; ++i) planes.emplace_back(region.shape);
}

std::size_t SwathRasterBlock::bytes() const { return block_bytes(window.shape,planes.size()); }

RasterizedFrame::RasterizedFrame(const RasterOptions& options, ProfileReport& report)
    : framebuffer_memory(report.reserve(BufferKind::framebuffer, options.layout.framebuffer.elements() *
                                         (options.bit_depth == 8 ? sizeof(Pixel8) : sizeof(Pixel16)))),
      layout(options.layout),
      pixels(options.bit_depth == 8 ? std::variant<Raster<Pixel8>,Raster<Pixel16>>(
                 std::in_place_type<Raster<Pixel8>>, options.layout.framebuffer) :
                 std::variant<Raster<Pixel8>,Raster<Pixel16>>(
                 std::in_place_type<Raster<Pixel16>>, options.layout.framebuffer)) {
    if (options.keep_coverage) {
        coverage_memory.emplace(report.reserve(BufferKind::ownership, options.layout.map.elements() * sizeof(float)));
        coverage.emplace(options.layout.map);
    }
}

RasterizedFrame rasterize_swaths(std::span<const SwathRasterSource> sources,
                                 const RasterOptions& options, ExecutionContext& context) {
    if (sources.empty() || !options.tile_size || (options.bit_depth != 8 && options.bit_depth != 16) ||
        !options.layout.map.elements() || !options.layout.framebuffer.elements())
        throw std::invalid_argument("Invalid rasterization request");
    const auto shape = options.layout.map;
    if (options.layout.left > options.layout.framebuffer.cols || options.layout.top > options.layout.framebuffer.rows ||
        shape.cols > options.layout.framebuffer.cols - options.layout.left ||
        shape.rows > options.layout.framebuffer.rows - options.layout.top)
        throw std::invalid_argument("Map lies outside framebuffer");
    std::vector<const SwathRasterSource*> ordered;
    for (const auto& source : sources) { source.validate(); ordered.push_back(&source); }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->scene_order < b->scene_order; });
    std::vector<std::size_t> prefix{0};
    for (const auto* source : ordered) {
        if (source->display.color_space != ordered.front()->display.color_space)
            throw std::invalid_argument("Cannot mix color spaces in one framebuffer");
        const auto count = source->cells.elements();
        if (count > std::numeric_limits<std::uint32_t>::max() - prefix.back())
            throw std::overflow_error("Source exceeds compact reference capacity");
        prefix.push_back(prefix.back() + count);
    }
    const auto tile_cols = shape.cols / options.tile_size + (shape.cols % options.tile_size != 0);
    const auto tile_rows = shape.rows / options.tile_size + (shape.rows % options.tile_size != 0);
    const auto tile_count = Shape{tile_rows,tile_cols}.elements();
    auto offset_memory = context.profile().reserve(BufferKind::bins, (tile_count + 1) * sizeof(std::size_t) * 2);
    std::vector<std::size_t> offsets(tile_count + 1, 0);
    auto visit_bins = [&](auto&& visit) {
        SourceReader reader(options.source_cache_bytes);
        for (std::size_t scene = 0; scene < ordered.size(); ++scene) {
            const auto& source = *ordered[scene];
            auto visit_cell = [&](std::size_t index) {
                const auto cell = reader.cell(source,index);
                auto box = extent(*cell.source,cell.index,shape);
                if (!box) return;
                const auto first_row = box->top / options.tile_size, last_row = (box->bottom - 1) / options.tile_size;
                const auto first_col = box->left / options.tile_size, last_col = (box->right - 1) / options.tile_size;
                const auto key = reference_key(source,index,static_cast<std::uint32_t>(prefix[scene] + index),options.overlap);
                for (auto row = first_row; row <= last_row; ++row)
                    for (auto col = first_col; col <= last_col; ++col) visit(row * tile_cols + col, key);
            };
            if (source.read_block)
                for (std::size_t row = 0; row < source.cells.rows; row += source.block_size)
                    for (std::size_t col = 0; col < source.cells.cols; col += source.block_size)
                        for (auto r = row; r < std::min(row + source.block_size,source.cells.rows); ++r)
                            for (auto c = col; c < std::min(col + source.block_size,source.cells.cols); ++c)
                                visit_cell(r * source.cells.cols + c);
            else
                for (std::size_t index = 0; index < source.cells.elements(); ++index) visit_cell(index);
        }
    };
    {
        auto phase = context.profile().phase("bin_counts");
        visit_bins([&](std::size_t tile, std::uint64_t) {
            if (offsets[tile + 1] == std::numeric_limits<std::size_t>::max()) throw std::overflow_error("Bin count overflow");
            ++offsets[tile + 1];
        });
        for (std::size_t i = 1; i < offsets.size(); ++i) {
            if (offsets[i] > std::numeric_limits<std::size_t>::max() - offsets[i-1]) throw std::overflow_error("Bin extent overflow");
            offsets[i] += offsets[i-1];
        }
    }
    if (offsets.back() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t))
        throw std::overflow_error("Bin allocation overflow");
    auto bin_memory = context.profile().reserve(BufferKind::bins, offsets.back() * sizeof(std::uint64_t));
    std::vector<std::uint64_t> references(offsets.back());
    auto cursor = offsets;
    {
        auto phase = context.profile().phase("bin_fill");
        visit_bins([&](std::size_t tile, std::uint64_t key) { references[cursor[tile]++] = key; });
    }
    RasterizedFrame result(options, context.profile());
    {
        auto phase = context.profile().phase("rasterization");
        context.parallel_rows(tile_count, [&](std::size_t begin, std::size_t end) {
            SourceReader reader(options.source_cache_bytes);
            for (auto tile = begin; tile < end; ++tile) {
                if (offsets[tile] == offsets[tile + 1]) continue;
                auto candidates = std::span(references).subspan(offsets[tile], offsets[tile + 1] - offsets[tile]);
                if (options.overlap == OverlapPolicy::last_sample &&
                    std::ranges::none_of(ordered,[](const auto* source) { return bool(source->read_block); }))
                    std::reverse(candidates.begin(),candidates.end());
                else std::sort(candidates.begin(),candidates.end(),std::greater<>{});
                const auto left = tile % tile_cols * options.tile_size, top = tile / tile_cols * options.tile_size;
                const auto width = std::min(options.tile_size,shape.cols - left), height = std::min(options.tile_size,shape.rows - top);
                TrackedMemory tracked(context.profile());
                std::pmr::unsynchronized_pool_resource pool(&tracked);
                std::pmr::vector<PixelWork> pixels(&pool);
                pixels.reserve(width * height);
                for (std::size_t i = 0; i < width * height; ++i) pixels.emplace_back(&pool);
                std::pmr::vector<float> values(&pool);
                for (auto key : candidates) {
                    const auto global = reference_index(key,options.overlap);
                    const auto found = std::upper_bound(prefix.begin(),prefix.end(),global);
                    const auto scene = static_cast<std::size_t>(found - prefix.begin() - 1);
                    const auto& source = *ordered[scene];
                    const auto index = global - prefix[scene];
                    const auto cell = reader.cell(source,index);
                    const auto& view = *cell.source;
                    const auto row = cell.index / view.cells.cols, col = cell.index % view.cells.cols;
                    values.resize(view.planes.size());
                    bool finite = true;
                    for (std::size_t i = 0; i < values.size(); ++i) {
                        values[i] = view.planes[i].at(row,col);
                        finite = finite && std::isfinite(values[i]);
                    }
                    if (!finite) continue;
                    auto color = source.display.mapper(values,source.display.scale);
                    if (!std::isfinite(color.red) || !std::isfinite(color.green) || !std::isfinite(color.blue) ||
                        !std::isfinite(color.alpha) || color.alpha <= 0) continue;
                    color.red = std::clamp(color.red,0.0,1.0);
                    color.green = std::clamp(color.green,0.0,1.0);
                    color.blue = std::clamp(color.blue,0.0,1.0);
                    color.alpha = std::clamp(color.alpha,0.0,1.0);
                    const auto quad = quad_at(view,cell.index);
                    const auto box = extent(view,cell.index,shape).value();
                    for (auto y = std::max(top,box.top); y < std::min(top + height,box.bottom); ++y)
                        for (auto x = std::max(left,box.left); x < std::min(left + width,box.right); ++x)
                            contribute(pixels[(y - top) * width + x - left],quad,x,y,color,&pool);
                }
                std::visit([&](auto& image) {
                    using Component = std::remove_reference_t<decltype(image.view().at(0,0).bgr[0])>;
                    constexpr auto maximum = std::numeric_limits<Component>::max();
                    for (std::size_t row = 0; row < height; ++row)
                        for (std::size_t col = 0; col < width; ++col) {
                            const auto& pixel = pixels[row * width + col];
                            auto& output = image.view().at(options.layout.top + top + row,options.layout.left + left + col);
                            output.bgr = {static_cast<Component>(std::lround(std::clamp(pixel.sum.blue,0.0,1.0) * maximum)),
                                          static_cast<Component>(std::lround(std::clamp(pixel.sum.green,0.0,1.0) * maximum)),
                                          static_cast<Component>(std::lround(std::clamp(pixel.sum.red,0.0,1.0) * maximum))};
                            if (result.coverage) result.coverage->view().at(top + row,left + col) =
                                static_cast<float>(std::clamp(pixel.coverage,0.0,1.0));
                        }
                },result.pixels);
            }
        },1);
    }
    context.profile().set_output(options.layout.framebuffer);
    return result;
}

}
