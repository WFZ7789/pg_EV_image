/// SPDX-License-Identifier: MIT

#include "core/resample.hpp"

#include <cmath>

namespace {

void require(bool condition) {
    if (!condition) throw std::runtime_error("Resampling check failed");
}

}

int main() {
    using namespace fy::core;
    ExecutionContext context({.threads = 4});
    Shape shape{5, 7}, target{20, 28};
    Raster<float> values(shape);
    Raster<std::uint8_t> valid(shape);
    for (std::size_t row = 0; row < shape.rows; ++row)
        for (std::size_t col = 0; col < shape.cols; ++col) {
            values.view().at(row, col) = static_cast<float>(row * row + col * 3);
            valid.view().at(row, col) = 1;
        }
    auto whole = resample_cubic(values.view(), valid.view(), {0, 0, shape}, shape,
                                {0, 0, target}, target, false, context);
    for (std::size_t row = 0; row < target.rows; row += 3) {
        Window output{row, 0, {std::min<std::size_t>(3, target.rows - row), target.cols}};
        auto input = cubic_source_window(shape, target, output);
        auto part = resample_cubic(values.view().window(input), valid.view().window(input), input,
                                   shape, output, target, false, context);
        for (std::size_t r = 0; r < output.shape.rows; ++r)
            for (std::size_t c = 0; c < output.shape.cols; ++c)
                require(part.values.view().at(r, c) == whole.values.view().at(row + r, c));
    }
    auto identity = resample_cubic(values.view(), valid.view(), {0, 0, shape}, shape,
                                   {0, 0, shape}, shape, false, context);
    for (std::size_t i = 0; i < shape.elements(); ++i) require(identity.values.values()[i] == values.values()[i]);
    values.view().at(2, 3) = -9999.9f;
    valid.view().at(2, 3) = 0;
    auto masked = resample_cubic(values.view(), valid.view(), {0, 0, shape}, shape,
                                 {0, 0, target}, target, false, context);
    std::size_t invalid = 0;
    for (std::size_t i = 0; i < target.elements(); ++i) {
        if (!masked.valid.values()[i]) { ++invalid; require(std::isnan(masked.values.values()[i])); }
        else require(std::abs(masked.values.values()[i]) < 100);
    }
    require(invalid > 0 && invalid < target.elements());
    auto retained = resample_cubic(values.view(),valid.view(),{0,0,shape},shape,{0,0,target},target,
                                   false,context,InvalidSamplePolicy::retain_finite);
    std::fill(valid.values().begin(),valid.values().end(),1);
    auto reference = resample_cubic(values.view(),valid.view(),{0,0,shape},shape,{0,0,target},target,false,context);
    for (std::size_t i = 0; i < target.elements(); ++i) {
        require(retained.valid.values()[i] == masked.valid.values()[i]);
        require(retained.values.values()[i] == reference.values.values()[i]);
    }
    std::fill(valid.values().begin(), valid.values().end(), 0);
    auto empty = resample_cubic(values.view(), valid.view(), {0,0,shape}, shape,
                                {0,0,target}, target, false, context);
    for (auto value : empty.valid.values()) require(value == 0);
    SwathGeometry geo{Raster<float>({2, 2}, {10,10,11,11}), Raster<float>({2, 2}, {179,-179,179,-179}),
                       Raster<std::uint8_t>({2, 2}, {1,1,1,1})};
    auto crossing = resample_geolocation(geo, {0,0,{2,2}}, {2,2}, {0,0,{8,8}}, {8,8}, context);
    crossing.validate();
    for (float longitude : crossing.longitude.values()) require(std::abs(longitude) > 178);
    bool rejected = false;
    try {
        resample_cubic(values.view().window({0,0,{1,1}}), valid.view().window({0,0,{1,1}}),
                        {0,0,{1,1}}, shape, {5,5,{5,5}}, target, false, context);
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
}
