/// SPDX-License-Identifier: MIT

#include "products/cache.hpp"

#include <algorithm>
#include <fstream>

namespace fy::products {
namespace {

using namespace core;

Shape checked_shape(const LoadedProduct& product) {
    if (!product.product.frame.geometry) throw std::invalid_argument("Cached product has no geometry");
    return geometry_shape(*product.product.frame.geometry);
}

class CacheFile {
public:
    CacheFile(std::filesystem::path path,ProfileReport& profile) : path_(std::move(path)),profile_(profile) {}
    ~CacheFile() { clear(); }
    CacheFile(const CacheFile&) = delete;
    CacheFile& operator=(const CacheFile&) = delete;

    std::ofstream create() {
        if (owned_ || std::filesystem::exists(path_)) throw std::invalid_argument("Cache file already exists");
        std::ofstream output(path_,std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit); owned_ = true;
        return output;
    }
    std::ifstream open() const {
        if (!owned_) throw std::logic_error("Cache file is unavailable");
        std::ifstream input(path_,std::ios::binary);
        input.exceptions(std::ios::badbit | std::ios::failbit);
        if (std::filesystem::file_size(path_) != bytes_) throw std::runtime_error("Cache file size changed");
        return input;
    }
    template<class T> void write(std::ofstream& output,std::span<const T> values) {
        const auto bytes = values.size_bytes();
        output.write(reinterpret_cast<const char*>(values.data()),static_cast<std::streamsize>(bytes));
        bytes_ += bytes; profile_.record_cache_write(bytes);
    }
    template<class T> void read(std::ifstream& input,std::span<T> values) const {
        const auto bytes = values.size_bytes();
        input.read(reinterpret_cast<char*>(values.data()),static_cast<std::streamsize>(bytes));
        profile_.record_cache_read(bytes);
    }
    void clear() noexcept {
        if (!owned_) return;
        std::error_code error;
        std::filesystem::remove(path_,error);
        if (!error) { profile_.release_cache(bytes_); bytes_ = 0; owned_ = false; }
    }
    std::uint64_t bytes() const { return bytes_; }
private:
    std::filesystem::path path_;
    ProfileReport& profile_;
    std::uint64_t bytes_ = 0;
    bool owned_ = false;
};

template<class T>
void copy_region(RasterView<const T> source,Window window,std::vector<T>& output) {
    output.resize(window.shape.elements());
    for (std::size_t row = 0; row < window.shape.rows; ++row)
        std::copy_n(source.row(window.row + row).begin() + window.col,window.shape.cols,
                    output.begin() + row * window.shape.cols);
}

}

struct CachedSwath::Impl {
    struct Block { Window window; std::uint64_t values = 0,vertices = 0; };
    ExecutionContext& context;
    CacheFile geometry,numeric,projected;
    Shape shape,source_shape;
    Window source_window;
    std::size_t block_size,columns,plane_count,scan_rows = 0,first_source_row = 0;
    std::vector<Block> blocks;
    DisplayDescriptor display;
    std::vector<std::string> labels;
    std::string extra;
    bool ready = false;

    Impl(const std::filesystem::path& prefix,const LoadedProduct& product,ExecutionContext& execution,std::size_t block)
        : context(execution),geometry(prefix.string() + ".geo",execution.profile()),
          numeric(prefix.string() + ".values",execution.profile()),projected(prefix.string() + ".vertices",execution.profile()),
          shape(checked_shape(product)),source_shape(product.source_shape),source_window(product.source_window),
          block_size(block),columns(block ? shape.cols / block + (shape.cols % block != 0) : 0),
          plane_count(product.product.frame.planes.size()),display(product.product.display),labels(product.product.labels),extra(product.product.extra_label) {
        if (!block_size || !plane_count) throw std::invalid_argument("Invalid swath cache block size");
        product.product.frame.validate();
        const auto& native = std::get<SwathGeometry>(*product.product.frame.geometry);
        scan_rows = native.scan_rows; first_source_row = native.first_source_row;
        if (scan_rows && (block_size != scan_rows || first_source_row%scan_rows || shape.rows%scan_rows))
            throw std::invalid_argument("Scan cache requires complete scan-aligned blocks");
        auto phase = context.profile().phase("cache_write");
        {
            auto output = geometry.create();
            const auto& swath = std::get<SwathGeometry>(*product.product.frame.geometry);
            geometry.write(output,swath.latitude.values()); geometry.write(output,swath.longitude.values()); geometry.write(output,swath.valid.values());
            output.close();
        }
        const Shape scratch{std::min(block_size,shape.rows),std::min(block_size,shape.cols)};
        auto memory = context.profile().reserve(BufferKind::statistics,scratch.elements() * 5);
        std::vector<float> values; values.reserve(scratch.elements());
        std::vector<std::uint8_t> valid; valid.reserve(scratch.elements());
        auto output = numeric.create();
        for (std::size_t row = 0; row < shape.rows; row += block_size)
            for (std::size_t col = 0; col < shape.cols; col += block_size) {
                Window window{row,col,{std::min(block_size,shape.rows - row),std::min(block_size,shape.cols - col)}};
                blocks.push_back({window,numeric.bytes()});
                valid.assign(window.shape.elements(),1);
                for (const auto& plane : product.product.frame.planes)
                    for (std::size_t r = 0; r < window.shape.rows; ++r)
                        for (std::size_t c = 0; c < window.shape.cols; ++c)
                            valid[r * window.shape.cols + c] &= plane.valid.view().at(row + r,col + c);
                numeric.write(output,std::span<const std::uint8_t>(valid));
                for (const auto& plane : product.product.frame.planes) {
                    copy_region(plane.values.view(),window,values); numeric.write(output,std::span<const float>(values));
                }
            }
        output.close(); context.profile().record_cached_scene();
    }

    std::unique_ptr<SwathRasterBlock> read(Window window) const {
        if (!ready || window.row % block_size || window.col % block_size) throw std::invalid_argument("Invalid cached source window");
        window.validate(shape);
        const auto& entry = blocks.at(window.row / block_size * columns + window.col / block_size);
        if (entry.window.shape != window.shape) throw std::invalid_argument("Cached source block extent mismatch");
        auto result = std::make_unique<SwathRasterBlock>(window,plane_count,context.profile());
        auto input = projected.open(); input.seekg(static_cast<std::streamoff>(entry.vertices));
        projected.read(input,result->vertices.values());
        auto numbers = numeric.open(); numbers.seekg(static_cast<std::streamoff>(entry.values));
        numeric.read(numbers,result->valid.values());
        for (auto& plane : result->planes) numeric.read(numbers,plane.values());
        return result;
    }
};

CachedSwath::CachedSwath(const std::filesystem::path& prefix,const LoadedProduct& product,ExecutionContext& context,std::size_t block_size)
    : impl_(std::make_unique<Impl>(prefix,product,context,block_size)) {}
CachedSwath::~CachedSwath() = default;
DisplayDescriptor& CachedSwath::display() { return impl_->display; }
const std::vector<std::string>& CachedSwath::labels() const { return impl_->labels; }
const std::string& CachedSwath::extra_label() const { return impl_->extra; }

void CachedSwath::project(const MapView& map) {
    auto& data = *impl_;
    if (data.ready) throw std::logic_error("Cached source has already been projected");
    auto phase = data.context.profile().phase("cache_projection");
    auto memory = data.context.profile().reserve(BufferKind::scene,data.shape.elements() * 9);
    SwathGeometry geometry{Raster<float>(data.shape),Raster<float>(data.shape),Raster<std::uint8_t>(data.shape)};
    geometry.scan_rows = data.scan_rows; geometry.first_source_row = data.first_source_row;
    {
        auto input = data.geometry.open();
        data.geometry.read(input,geometry.latitude.values()); data.geometry.read(input,geometry.longitude.values()); data.geometry.read(input,geometry.valid.values());
    }
    auto scratch_memory = data.context.profile().reserve(BufferKind::statistics,Shape{std::min(data.block_size,data.shape.rows) + 1,
                                                                                       std::min(data.block_size,data.shape.cols) + 1}.elements() * sizeof(PixelPoint));
    std::vector<PixelPoint> scratch;
    scratch.reserve(Shape{std::min(data.block_size,data.shape.rows) + 1,std::min(data.block_size,data.shape.cols) + 1}.elements());
    auto output = data.projected.create();
    if (data.scan_rows) {
        for (std::size_t row = 0; row < data.shape.rows; row += data.scan_rows) {
            auto strip_memory = data.context.profile().reserve(BufferKind::scene,data.scan_rows*data.shape.cols*9);
            auto strip = subset_swath_geometry(geometry,{row,0,{data.scan_rows,data.shape.cols}});
            auto projected = project_swath(strip,map,data.context);
            for (std::size_t col = 0; col < data.columns; ++col) {
                auto& block = data.blocks.at(row/data.block_size*data.columns+col);
                block.vertices = data.projected.bytes();
                copy_region<PixelPoint>(projected.vertices.view(),{0,block.window.col,
                    {block.window.shape.rows+1,block.window.shape.cols+1}},scratch);
                data.projected.write(output,std::span<const PixelPoint>(scratch));
            }
        }
    } else {
        auto projected = project_swath(geometry,map,data.context);
        for (auto& block : data.blocks) {
            block.vertices = data.projected.bytes();
            copy_region<PixelPoint>(projected.vertices.view(),{block.window.row,block.window.col,
                                      {block.window.shape.rows + 1,block.window.shape.cols + 1}},scratch);
            data.projected.write(output,std::span<const PixelPoint>(scratch));
        }
    }
    output.close(); data.ready = true; data.geometry.clear();
}

SwathRasterSource CachedSwath::source(std::uint64_t order) const {
    if (!impl_->ready) throw std::logic_error("Cached source has not been projected");
    SwathRasterSource result;
    result.cells = impl_->shape; result.display = impl_->display; result.scene_order = order;
    result.source_column = impl_->source_window.col; result.source_columns = impl_->source_shape.cols;
    result.block_size = impl_->block_size; result.plane_count = impl_->plane_count;
    result.read_block = [this](Window window) { return impl_->read(window); };
    return result;
}

std::vector<SwathRasterSource> CachedSwath::sources(std::uint64_t order) const {
    if (!impl_->scan_rows) return {source(order)};
    std::vector<SwathRasterSource> result;
    for (std::size_t row = 0; row < impl_->shape.rows; row += impl_->scan_rows) {
        auto strip = source(order+result.size());
        strip.cells.rows = impl_->scan_rows;
        strip.read_block = [this,row](Window window) {
            auto block = impl_->read({row+window.row,window.col,window.shape});
            block->window = window;
            return block;
        };
        result.push_back(std::move(strip));
    }
    return result;
}

}
