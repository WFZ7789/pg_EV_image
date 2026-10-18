/// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace fy::core {

struct Shape {
    std::size_t rows = 0;
    std::size_t cols = 0;
    bool operator==(const Shape&) const = default;

    std::size_t elements() const {
        if ((rows == 0) != (cols == 0)) throw std::invalid_argument("Partial empty shape");
        if (cols && rows > std::numeric_limits<std::size_t>::max() / cols)
            throw std::overflow_error("Raster size overflow");
        return rows * cols;
    }
};

struct Window {
    std::size_t row = 0;
    std::size_t col = 0;
    Shape shape;

    void validate(Shape source) const {
        source.elements();
        shape.elements();
        if (row > source.rows || col > source.cols ||
            shape.rows > source.rows - row || shape.cols > source.cols - col)
            throw std::out_of_range("Window outside source");
    }
};

/// @note 所有者必须存活至消费完成；行跨度以元素计。
template<class T>
class RasterView {
public:
    RasterView() = default;
    RasterView(std::span<T> storage, Shape shape, std::size_t stride)
        : storage_(storage), shape_(shape), stride_(stride) {
        const auto count = shape.elements();
        if (!count) {
            if (!storage.empty()) throw std::invalid_argument("Nonempty storage for empty view");
            return;
        }
        if (stride < shape.cols) throw std::invalid_argument("Row stride too small");
        if (shape.rows - 1 > (std::numeric_limits<std::size_t>::max() - shape.cols) / stride)
            throw std::overflow_error("View extent overflow");
        if (storage.size() < (shape.rows - 1) * stride + shape.cols)
            throw std::invalid_argument("View storage too small");
        storage_ = storage.first((shape.rows - 1) * stride + shape.cols);
    }

    template<class U> requires std::is_convertible_v<U(*)[], T(*)[]>
    RasterView(const RasterView<U>& other)
        : RasterView(other.storage(), other.shape(), other.stride()) {}

    Shape shape() const noexcept { return shape_; }
    std::size_t stride() const noexcept { return stride_; }
    std::span<T> storage() const noexcept { return storage_; }

    std::span<T> row(std::size_t index) const {
        if (index >= shape_.rows) throw std::out_of_range("Row outside view");
        return storage_.subspan(index * stride_, shape_.cols);
    }

    T& at(std::size_t row_index, std::size_t col_index) const {
        if (col_index >= shape_.cols) throw std::out_of_range("Column outside view");
        return row(row_index)[col_index];
    }

    RasterView window(Window region) const {
        region.validate(shape_);
        if (!region.shape.elements()) return {};
        const auto extent = (region.shape.rows - 1) * stride_ + region.shape.cols;
        return {storage_.subspan(region.row * stride_ + region.col, extent), region.shape, stride_};
    }

private:
    std::span<T> storage_;
    Shape shape_;
    std::size_t stride_ = 0;
};

template<class T>
class Raster {
public:
    Raster() = default;
    explicit Raster(Shape shape) : shape_(shape), values_(shape.elements()) {}
    Raster(Shape shape, std::vector<T>&& values) : shape_(shape), values_(std::move(values)) {
        if (values_.size() != shape.elements()) throw std::invalid_argument("Raster length mismatch");
    }
    Raster(const Raster&) = delete;
    Raster& operator=(const Raster&) = delete;
    Raster(Raster&& other) noexcept
        : shape_(std::exchange(other.shape_, {})), values_(std::move(other.values_)) {
        other.values_.clear();
    }
    Raster& operator=(Raster&& other) noexcept {
        if (this != &other) {
            shape_ = std::exchange(other.shape_, {});
            values_ = std::move(other.values_);
            other.values_.clear();
        }
        return *this;
    }

    Shape shape() const noexcept { return shape_; }
    std::size_t bytes() const noexcept { return values_.size() * sizeof(T); }
    std::span<T> values() & noexcept { return values_; }
    std::span<const T> values() const & noexcept { return values_; }
    std::span<T> values() && = delete;
    std::span<const T> values() const && = delete;
    RasterView<T> view() & { return {values_, shape_, shape_.cols}; }
    RasterView<const T> view() const & { return {values_, shape_, shape_.cols}; }
    RasterView<T> view() && = delete;
    RasterView<const T> view() const && = delete;

private:
    Shape shape_;
    std::vector<T> values_;
};

}
