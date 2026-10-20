/// SPDX-License-Identifier: MIT

#include "core/resample.hpp"

#include <algorithm>
#include <cmath>

namespace fy::core {
namespace {

struct Coordinate {
    std::int64_t base;
    std::array<float, 4> weights;
};

struct PreparedCoordinate {
    std::array<std::size_t,4> samples;
    std::array<float,4> weights;
    std::size_t anchor;
};

Coordinate coordinate(std::size_t index, std::size_t source, std::size_t target) {
    const double position = (static_cast<double>(index) + 0.5) * static_cast<double>(source) / target - 0.5;
    const auto base = static_cast<std::int64_t>(std::floor(position));
    const float x = static_cast<float>(position - base);
    constexpr float a = -0.75f;
    const float y = 1 - x;
    std::array<float, 4> weights;
    weights[0] = ((a * (x + 1) - 5 * a) * (x + 1) + 8 * a) * (x + 1) - 4 * a;
    weights[1] = ((a + 2) * x - (a + 3)) * x * x + 1;
    weights[2] = ((a + 2) * y - (a + 3)) * y * y + 1;
    weights[3] = 1 - weights[0] - weights[1] - weights[2];
    return {base, weights};
}

void validate_shape(Shape shape) {
    if (!shape.elements() || shape.rows > std::numeric_limits<int>::max() ||
        shape.cols > std::numeric_limits<int>::max())
        throw std::invalid_argument("Unsupported resampling shape");
}

std::size_t bounded(std::int64_t value, std::size_t size) {
    return static_cast<std::size_t>(std::clamp<std::int64_t>(value, 0, static_cast<std::int64_t>(size) - 1));
}

PreparedCoordinate prepare(std::size_t index,std::size_t source,std::size_t target,std::size_t offset) {
    const auto value = coordinate(index,source,target);
    PreparedCoordinate result{.weights = value.weights,
                              .anchor = bounded(value.base,source) - offset};
    for (int i = 0; i < 4; ++i) result.samples[i] = bounded(value.base + i - 1,source) - offset;
    return result;
}

}

Window cubic_source_window(Shape source, Shape target, Window output) {
    validate_shape(source); validate_shape(target);
    output.validate(target);
    if (!output.shape.elements()) throw std::invalid_argument("Empty resampling window");
    const auto first_row = coordinate(output.row, source.rows, target.rows).base;
    const auto last_row = coordinate(output.row + output.shape.rows - 1, source.rows, target.rows).base;
    const auto first_col = coordinate(output.col, source.cols, target.cols).base;
    const auto last_col = coordinate(output.col + output.shape.cols - 1, source.cols, target.cols).base;
    const auto row = bounded(first_row - 1, source.rows), col = bounded(first_col - 1, source.cols);
    const auto end_row = bounded(last_row + 2, source.rows), end_col = bounded(last_col + 2, source.cols);
    return {row, col, {end_row - row + 1, end_col - col + 1}};
}

ScalarPlane resample_cubic(RasterView<const float> values, RasterView<const std::uint8_t> valid,
                           Window input, Shape source, Window output, Shape target,
                           bool longitude, ExecutionContext& context, InvalidSamplePolicy invalid) {
    const auto needed = cubic_source_window(source, target, output);
    input.validate(source);
    if (values.shape() != input.shape || valid.shape() != input.shape ||
        input.row > needed.row || input.col > needed.col ||
        input.row + input.shape.rows < needed.row + needed.shape.rows ||
        input.col + input.shape.cols < needed.col + needed.shape.cols)
        throw std::invalid_argument("Missing cubic interpolation halo");
    auto phase = context.profile().phase("resampling");
    ScalarPlane result{longitude ? "degree" : "1", Raster<float>(output.shape), Raster<std::uint8_t>(output.shape)};
    auto result_values = result.values.view();
    auto result_valid = result.valid.view();
    std::vector<PreparedCoordinate> rows(output.shape.rows),columns(output.shape.cols);
    for (std::size_t row = 0; row < rows.size(); ++row)
        rows[row] = prepare(row + output.row,source.rows,target.rows,input.row);
    for (std::size_t col = 0; col < columns.size(); ++col)
        columns[col] = prepare(col + output.col,source.cols,target.cols,input.col);
    if (!longitude) {
        context.parallel_rows(output.shape.rows,[&](std::size_t begin,std::size_t end) {
            std::size_t first = input.shape.rows,last = 0;
            for (auto row = begin; row < end; ++row)
                for (const auto sample : rows[row].samples) {
                    first = std::min(first,sample);
                    last = std::max(last,sample);
                }
            const auto source_rows = last - first + 1;
            const auto samples = source_rows * output.shape.cols;
            auto workspace = context.profile().reserve(BufferKind::worker_tiles,samples * 6);
            std::vector<float> horizontal(samples);
            std::vector<std::uint8_t> horizontal_good(samples),horizontal_finite(samples);
            for (std::size_t source_row = first; source_row <= last; ++source_row)
                for (std::size_t col = 0; col < output.shape.cols; ++col) {
                    const auto& x = columns[col];
                    float sum = 0;
                    bool good = true,finite = true;
                    for (int dx = 0; dx < 4; ++dx) {
                        if (x.weights[dx] == 0) continue;
                        const float value = values.at(source_row,x.samples[dx]);
                        if (!std::isfinite(value)) { good = false; finite = false; continue; }
                        if (!valid.at(source_row,x.samples[dx])) {
                            good = false;
                            if (invalid == InvalidSamplePolicy::nan) continue;
                        }
                        sum += value * x.weights[dx];
                    }
                    const auto index = (source_row - first) * output.shape.cols + col;
                    horizontal[index] = sum;
                    horizontal_good[index] = good;
                    horizontal_finite[index] = finite;
                }
            for (auto row = begin; row < end; ++row) {
                const auto& y = rows[row];
                for (std::size_t col = 0; col < output.shape.cols; ++col) {
                    float sum = 0;
                    bool good = true,finite = true;
                    for (int dy = 0; dy < 4; ++dy) {
                        if (y.weights[dy] == 0) continue;
                        const auto index = (y.samples[dy] - first) * output.shape.cols + col;
                        sum += horizontal[index] * y.weights[dy];
                        good = good && horizontal_good[index];
                        finite = finite && horizontal_finite[index];
                    }
                    finite = finite && std::isfinite(sum);
                    good = good && finite;
                    result_values.at(row,col) = finite && (good || invalid == InvalidSamplePolicy::retain_finite) ? sum : NAN;
                    result_valid.at(row,col) = good;
                }
            }
        },32);
        context.profile().record_computed(output.shape.elements());
        return result;
    }
    context.parallel_rows(output.shape.rows, [&](std::size_t begin, std::size_t end) {
        for (auto row = begin; row < end; ++row) {
            const auto& y = rows[row];
            for (std::size_t col = 0; col < output.shape.cols; ++col) {
                const auto& x = columns[col];
                const float anchor = values.at(y.anchor,x.anchor);
                float sum = 0;
                bool good = true, finite = true;
                for (int dy = 0; dy < 4; ++dy) {
                    if (y.weights[dy] == 0) continue;
                    float horizontal = 0;
                    for (int dx = 0; dx < 4; ++dx) {
                        if (x.weights[dx] == 0) continue;
                        float value = values.at(y.samples[dy],x.samples[dx]);
                        if (!std::isfinite(value)) { good = false; finite = false; continue; }
                        if (!valid.at(y.samples[dy],x.samples[dx])) {
                            good = false;
                            if (invalid == InvalidSamplePolicy::nan) continue;
                        }
                        if (longitude) value = static_cast<float>(anchor + std::remainder(double(value) - anchor, 360.0));
                        horizontal += value * x.weights[dx];
                    }
                    sum += horizontal * y.weights[dy];
                }
                if (longitude && good) sum = static_cast<float>(std::remainder(double(sum), 360.0));
                finite = finite && std::isfinite(sum);
                good = good && finite;
                result_values.at(row, col) = finite && (good || invalid == InvalidSamplePolicy::retain_finite) ? sum : NAN;
                result_valid.at(row, col) = good;
            }
        }
    });
    context.profile().record_computed(output.shape.elements());
    return result;
}

SwathGeometry resample_geolocation(const SwathGeometry& geometry, Window input, Shape source,
                                   Window output, Shape target, ExecutionContext& context) {
    geometry.validate();
    auto latitude = resample_cubic(geometry.latitude.view(), geometry.valid.view(), input, source,
                                  output, target, false, context);
    auto longitude = resample_cubic(geometry.longitude.view(), geometry.valid.view(), input, source,
                                   output, target, true, context);
    auto valid = latitude.valid.values();
    for (std::size_t i = 0; i < valid.size(); ++i)
        valid[i] = valid[i] && longitude.valid.values()[i] &&
                   latitude.values.values()[i] >= -90 && latitude.values.values()[i] <= 90;
    return {std::move(latitude.values), std::move(longitude.values), std::move(latitude.valid)};
}

}
