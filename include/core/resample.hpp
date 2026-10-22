/// SPDX-License-Identifier: MIT

#pragma once

#include "core/execution.hpp"

namespace fy::core {

Window cubic_source_window(Shape source, Shape target, Window output);

/// @brief 使用全景坐标取样；带 halo 的源窗口可独立复现整景重采样。
ScalarPlane resample_cubic(RasterView<const float> values, RasterView<const std::uint8_t> valid,
                           Window input, Shape source, Window output, Shape target,
                           bool longitude, ExecutionContext& context,
                           InvalidSamplePolicy invalid = InvalidSamplePolicy::nan);

SwathGeometry resample_geolocation(const SwathGeometry& geometry, Window input, Shape source,
                                   Window output, Shape target, ExecutionContext& context);

struct GeographicWeights {
    std::array<std::uint32_t,3> indices{};
    std::array<float,3> weights{};
};

/// @brief 在单位球面源网格三角形内建立插值权重，覆盖外不外推。
Raster<GeographicWeights> geographic_weights(const SwathGeometry& source, const SwathGeometry& target,
                                            ExecutionContext& context);
ScalarPlane resample_geographic(const ScalarPlane& source, const Raster<GeographicWeights>& weights,
                               ExecutionContext& context);

}
