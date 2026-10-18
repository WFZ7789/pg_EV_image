/// SPDX-License-Identifier: MIT

#pragma once

#include "hdf_fixture.hpp"

namespace pmr_fixture {

inline void write(const std::filesystem::path& path,bool masked = false,bool sensor = false,
                  bool navigation = true,bool field = true,bool two_dimensional = false) {
    using namespace hdf_fixture;
    H5::H5File file(path.string(),H5F_ACC_TRUNC);
    metadata(file,"FY-3G","PMR","PMR KuR 5000M");
    if (!sensor) H5Adelete(file.getId(),"Sensor Identification Code");
    constexpr std::size_t rows = 12,cols = 16;
    if (navigation) {
        const hsize_t dimensions[]{rows,cols,2};
        const std::span<const hsize_t> shape(dimensions,two_dimensional ? 2 : 3);
        const auto channels = two_dimensional ? 1 : 2;
        std::vector<float> lat(rows * cols * channels,2000),lon(lat.size(),2000);
        for (std::size_t row = 0; row < rows; ++row)
            for (std::size_t col = 0; col < cols; ++col) {
                const auto i = (row * cols + col) * channels;
                lat[i] = float(15 + row);
                lon[i] = float(144 + col * 2);
            }
        if (masked) lat[0] = 20;
        auto latitude = dataset(file,"Geo_Fields/Latitude",shape,lat);
        auto longitude = dataset(file,"Geo_Fields/Longitude",shape,lon);
        hdf_fixture::numbers(latitude,"Slope",{.5}); hdf_fixture::numbers(latitude,"Intercept",{5});
        hdf_fixture::numbers(longitude,"Slope",{.25}); hdf_fixture::numbers(longitude,"Intercept",{100});
        if (masked) hdf_fixture::numbers(latitude,"_FillValue",{20});
    }
    if (field) {
        const hsize_t dimensions[]{rows,cols};
        std::vector<float> values(rows * cols);
        for (std::size_t row = 0; row < rows; ++row)
            for (std::size_t col = 0; col < cols; ++col)
                values[row * cols + col] = float(40 + row * 4 + col);
        values[0] = 1000;
        if (masked) values[5 * cols + 8] = 200;
        auto data = dataset(file,"SLV/zFactorCorrectedESurface",dimensions,values);
        hdf_fixture::numbers(data,"Slope",{.5}); hdf_fixture::numbers(data,"Intercept",{-10});
        text(data,"Long_Name","Corrected surface radar reflectivity");
        if (masked) hdf_fixture::numbers(data,"Fill_Value",{200});
        hdf_fixture::numbers(data,"Valid_Range",{0,1000});
    }
}

}
