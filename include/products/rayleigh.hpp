/// SPDX-License-Identifier: MIT

#pragma once

#include "core/execution.hpp"

namespace fy::products {

/// @brief Hansen–Travis 海平面标准气压光学厚度；波长单位 μm。
double rayleigh_optical_depth(double wavelength_um);

/// @brief Vermote–Tanré 分子路径反射率；相对方位角 0° 表示后向散射。
double rayleigh_path_reflectance(double optical_depth,double solar_cosine,double sensor_cosine,
                                 double relative_azimuth_deg);

/// @brief 对太阳角度校正后的 B/G/R 反射率扣除路径散射，亮云和低太阳高度处渐弱。
void correct_rayleigh(std::span<core::ScalarPlane> bgr,const core::ScalarPlane& solar_zenith,
                      const core::ScalarPlane& solar_azimuth,const core::ScalarPlane& sensor_zenith,
                      const core::ScalarPlane& sensor_azimuth,core::ExecutionContext& context);

}
