/// SPDX-License-Identifier: MIT

#pragma once
#include "raster.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fy {

struct Input {
    std::filesystem::path data;
    std::filesystem::path geo;
    std::filesystem::path aux;
};

struct Request {
    std::vector<Input> inputs;
    std::string mode;
    int band = 0;
    std::string color;
    std::filesystem::path output;
    std::string projection = "geographic";
    bool grid = false;
    double grid_step = 0;
    double center_lat = 0;
    double center_lon = 0;
    std::optional<double> half_range;
    int width = 0;
    std::optional<float> forced_scale;
};

struct ProjectedPoint {
    double x = 0;
    double y = 0;
    bool valid = false;
};

struct GeoProjection {
    bool orthographic = false;
    double center_lat = 0;
    double center_lon = 0;
    ProjectedPoint project(double lat, double lon) const;
};

struct Scene {
    ProductFrame image;
    std::string satellite;
    std::string sensor;
    std::string alias;
    std::string time_key;
    std::filesystem::path source;
};

void inspect(const std::filesystem::path& data);
void render(const Request& request);

}
