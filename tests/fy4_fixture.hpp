/// SPDX-License-Identifier: MIT

#pragma once

#include "hdf_fixture.hpp"

namespace fy4_fixture {

inline void write(const std::filesystem::path& path,const std::string& satellite,const std::string& sensor,
              std::size_t rows,std::size_t cols,bool masked,bool navigation = false) {
    using namespace hdf_fixture;
    H5::H5File file(path.string(),H5F_ACC_TRUNC);
    metadata(file,satellite,sensor,"4000M");
    hdf_fixture::numbers(file,"Begin Line Number",{0}); hdf_fixture::numbers(file,"Begin Pixel Number",{0});
    hdf_fixture::numbers(file,"End Line Number",{double(rows - 1)}); hdf_fixture::numbers(file,"End Pixel Number",{double(cols - 1)});
    hdf_fixture::numbers(file,sensor == "GHI" ? "Earth_Sun Distance Ratio" : "Earth/Sun Distance Ratio",{1.012});
    if (navigation) {
        hdf_fixture::numbers(file,"NOMCenterLat",{0}); hdf_fixture::numbers(file,"NOMCenterLon",{105});
        hdf_fixture::numbers(file,"NOMSatHeight",{35786000}); hdf_fixture::numbers(file,"dEA",{6377830});
        hdf_fixture::numbers(file,"dObRecFlat",{298.257223563});
        hdf_fixture::numbers(file,"dSamplingAngle",{1120}); hdf_fixture::numbers(file,"dSteppingAngle",{1120});
    }
    const hsize_t shape[]{rows,cols}, table_shape[]{64};
    for (int band = 1; band <= 14; ++band) {
        const auto suffix = (band < 10 ? "0" : "") + std::to_string(band);
        std::vector<float> values(rows * cols),coefficients(64);
        for (std::size_t i = 0; i < values.size(); ++i) values[i] = float((i / cols * 7 + i % cols * 3 + band * 5) % 64);
        if (masked) { values[0] = 65535; values[values.size() / 2] = 65535; }
        auto data = dataset(file,"Data/NOMChannel" + suffix,shape,values,H5::PredType::NATIVE_UINT16);
        hdf_fixture::numbers(data,"FillValue",{65535}); hdf_fixture::numbers(data,"valid_range",{0,63});
        for (std::size_t i = 0; i < coefficients.size(); ++i) coefficients[i] = band <= 6 ? 0.01f + float(i) * 0.011f : 190 + float(i) * 1.7f;
        auto table = dataset(file,"Calibration/CALChannel" + suffix,table_shape,coefficients);
        hdf_fixture::numbers(table,"FillValue",{-65535});
    }
    for (auto name : {"NOMSunZenith","NOMSunAzimuth","NOMSatelliteZenith","NOMSatelliteAzimuth"}) {
        std::vector<float> values(rows * cols);
        for (std::size_t row = 0; row < rows; ++row)
            for (std::size_t col = 0; col < cols; ++col)
                values[row * cols + col] = name == std::string("NOMSunZenith") ? float(20 + col * 90.0 / (cols - 1)) :
                                          name == std::string("NOMSunAzimuth") ? 45 : 25;
        auto data = dataset(file,std::string("Navigation/") + name,shape,values);
        hdf_fixture::numbers(data,"Slope",{1}); hdf_fixture::numbers(data,"Intercept",{0});
        hdf_fixture::numbers(data,"FillValue",{65535});
    }
}

}
