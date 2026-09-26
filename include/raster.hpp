/// SPDX-License-Identifier: MIT

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace fy {

template<class T>
struct Raster {
    int rows = 0;
    int cols = 0;
    std::vector<T> values;

    void validate() const {
        if (rows <= 0 || cols <= 0 || values.size() != size_t(rows) * cols)
            throw std::invalid_argument("栅格尺寸与数据长度不一致");
    }
};

struct GeoGrid {
    Raster<float> latitude;
    Raster<float> longitude;

    void validate() const {
        latitude.validate(); longitude.validate();
        if (latitude.rows != longitude.rows || latitude.cols != longitude.cols)
            throw std::invalid_argument("经纬度网格尺寸不一致");
    }
};

struct DisplayScale {
    float lowval = 0;
    float hival = 1;
};

struct ProductFrame {
    Raster<cv::Vec3b> color;
    GeoGrid geo;
    Raster<uint8_t> valid;
    std::vector<float> values;
    DisplayScale scale;
    bool has_colorbar = false;
    std::function<cv::Vec3b(double,const DisplayScale&)> mapper;

    void validate() const {
        color.validate(); geo.validate();
        if (color.rows != geo.latitude.rows || color.cols != geo.latitude.cols)
            throw std::invalid_argument("产品与地理网格尺寸不一致");
        if (!valid.values.empty()) {
            valid.validate();
            if (valid.rows != color.rows || valid.cols != color.cols)
                throw std::invalid_argument("有效掩膜与产品尺寸不一致");
        }
    }
};

}
