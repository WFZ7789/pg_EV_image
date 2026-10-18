/// SPDX-License-Identifier: MIT

#pragma once

#include "core/model.hpp"

namespace fy::core {

struct GeographicPoint {
    double latitude = 0;
    double longitude = 0;
    bool valid = false;
};

/// @brief 从已验证的圆盘几何计算经纬度；行列允许为像元边界处的小数。
GeographicPoint disk_geographic_at(const NativeDiskGeometry& geometry, double row, double col);

struct ImagePoint {
    double row = 0;
    double col = 0;
    bool valid = false;
};

ImagePoint disk_image_at(const NativeDiskGeometry& geometry, double latitude, double longitude);

}
