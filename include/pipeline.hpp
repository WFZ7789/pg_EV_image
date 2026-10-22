/// SPDX-License-Identifier: MIT

#pragma once
#include "core/execution.hpp"

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
    std::optional<std::string> amsr3_scan;
    std::filesystem::path output;
    std::string projection = "geographic";
    bool grid = false;
    double grid_step = 0;
    double center_lat = 0;
    double center_lon = 0;
    std::optional<double> half_range;
    int width = 0;
    core::ExecutionOptions execution;
};

void inspect(const std::filesystem::path& data);

}
