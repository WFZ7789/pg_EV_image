/// SPDX-License-Identifier: MIT

#include "core/navigation.hpp"

#include <cmath>
#include <numbers>

namespace fy::core {

GeographicPoint disk_geographic_at(const NativeDiskGeometry& geometry, double row, double col) {
    const auto x = geometry.scan_origin_rad[0] + col * geometry.scan_step_rad[0];
    const auto y = geometry.scan_origin_rad[1] + row * geometry.scan_step_rad[1];
    const auto a = geometry.equatorial_radius_m, b = geometry.polar_radius_m;
    const auto distance = geometry.satellite_distance_m;
    const auto cosx = std::cos(x), sinx = std::sin(x), cosy = std::cos(y), siny = std::sin(y);
    const auto ratio = a * a / (b * b);
    const auto coefficient = cosy * cosy + ratio * siny * siny;
    const auto discriminant = distance * distance * cosx * cosx * cosy * cosy -
                              coefficient * (distance * distance - a * a);
    if (!std::isfinite(discriminant) || discriminant < 0 || cosx * cosy <= 0) return {NAN,NAN,false};
    const auto ray = (distance * cosx * cosy - std::sqrt(discriminant)) / coefficient;
    const auto north = ray * siny, east = ray * sinx * cosy, radial = distance - ray * cosx * cosy;
    constexpr double degrees = 180 / std::numbers::pi;
    const auto latitude = std::atan2(north,std::hypot(east,radial) / ratio) * degrees;
    const auto longitude = std::remainder(geometry.longitude_deg + std::atan2(east,radial) * degrees,360);
    return {latitude,longitude,std::isfinite(latitude) && std::isfinite(longitude)};
}

ImagePoint disk_image_at(const NativeDiskGeometry& geometry,double latitude,double longitude) {
    if (!std::isfinite(latitude) || std::abs(latitude) > 90 || !std::isfinite(longitude)) return {NAN,NAN,false};
    constexpr double radians = std::numbers::pi / 180;
    const auto a = geometry.equatorial_radius_m, b = geometry.polar_radius_m;
    const auto phi = latitude * radians, lambda = std::remainder(longitude - geometry.longitude_deg,360) * radians;
    const auto ratio = b * b / (a * a);
    const auto radius = a / std::sqrt(1 - (1 - ratio) * std::sin(phi) * std::sin(phi));
    const auto north = radius * ratio * std::sin(phi), east = radius * std::cos(phi) * std::sin(lambda);
    const auto radial = radius * std::cos(phi) * std::cos(lambda);
    if (geometry.satellite_distance_m * radial / (a * a) <= 1) return {NAN,NAN,false};
    const auto distance = geometry.satellite_distance_m - radial;
    const auto x = std::atan2(east,distance), y = std::atan2(north,std::hypot(east,distance));
    const auto col = (x - geometry.scan_origin_rad[0]) / geometry.scan_step_rad[0];
    const auto row = (y - geometry.scan_origin_rad[1]) / geometry.scan_step_rad[1];
    return {row,col,std::isfinite(row) && std::isfinite(col)};
}

}
