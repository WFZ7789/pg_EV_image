/// SPDX-License-Identifier: MIT

#include "pipeline.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

int main() {
    auto require = [](bool condition) { if (!condition) throw std::runtime_error("投影检查失败"); };
    fy::GeoProjection geographic{false, 0, 179};
    auto west = geographic.project(12, -179);
    auto east = geographic.project(12, 181);
    require(west.valid && east.valid);
    require(std::abs(west.x - 181) < 1e-8);
    require(std::abs(west.x - east.x) < 1e-8);
    require(!geographic.project(91, 10).valid);
    require(!geographic.project(std::numeric_limits<double>::quiet_NaN(), 10).valid);

    fy::GeoProjection globe{true, 0, 0};
    auto center = globe.project(0, 0);
    auto horizon = globe.project(0, 90);
    require(center.valid && std::abs(center.x) < 1e-8 && std::abs(center.y) < 1e-8);
    require(horizon.valid && std::abs(horizon.x - 1) < 1e-8);
    require(!globe.project(0, 180).valid);
}
