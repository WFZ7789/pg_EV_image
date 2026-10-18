/// SPDX-License-Identifier: MIT

#pragma once

#include "hdf_fixture.hpp"

#include <cmath>

namespace giirs_fixture {

inline void write(const std::filesystem::path& path,bool masked = false,bool empty_channel = false,
                  bool empty_family = false,bool flat_table = false) {
    using namespace hdf_fixture;
    H5::H5File file(path.string(),H5F_ACC_TRUNC);
    metadata(file,"FY-4B","GIIRS","GIIRS_L1");
    for (auto [key,value] : {std::pair{"Begin Pixel Number",1}, {"Begin Line Number",1},
                             {"End Pixel Number",4}, {"End Line Number",6}})
        hdf_fixture::numbers(file,key,{double(value)});
    constexpr std::size_t rows = 4,cols = 6,pixels = rows * cols;
    for (auto [family,channels] : {std::pair{"LW",3}, {"MW",2}}) {
        const bool lw = std::string(family) == "LW";
        const hsize_t spatial[]{pixels},dims[]{static_cast<hsize_t>(channels),pixels},spectral[]{static_cast<hsize_t>(channels)};
        std::vector<float> lat(pixels),lon(pixels),raw(pixels * channels),wn(channels);
        for (std::size_t i = 0; i < pixels; ++i) {
            const auto row = i / cols;
            lat[i] = 10 + float(row) * .3f;
            lon[i] = 140 + float(i % cols) * .4f;
        }
        if (masked) lon[1] = 65535;
        for (int channel = 0; channel < channels; ++channel) {
            wn[channel] = lw ? float(700 + 50 * channel) : float(1600 + 100 * channel);
            for (std::size_t i = 0; i < pixels; ++i)
                raw[channel * pixels + i] = empty_family || (empty_channel && channel == 0) ? NAN :
                    lw ? 20 + channel * 15 + float(i) * .7f : .1f + channel * .4f + float(i) * .01f;
        }
        if (masked) raw[5] = 65535;
        auto latitude = dataset(file,std::string("Geolocation/Latitude_") + family,spatial,lat);
        auto longitude = dataset(file,std::string("Geolocation/Longitude_") + family,spatial,lon);
        for (auto* coordinate : {&latitude,&longitude}) {
            hdf_fixture::numbers(*coordinate,"FillValue",{65535});
            hdf_fixture::numbers(*coordinate,"Slope",{4}); hdf_fixture::numbers(*coordinate,"Intercept",{200});
        }
        auto radiance = dataset(file,std::string("Data/ES_Real") + family,dims,raw);
        hdf_fixture::numbers(radiance,"Slope",{2}); hdf_fixture::numbers(radiance,"Intercept",{.5});
        hdf_fixture::numbers(radiance,"FillValue",{65535}); hdf_fixture::numbers(radiance,"valid_range",{0,200});
        auto wavenumber = dataset(file,std::string("Data/WN_") + family,spectral,wn);
        hdf_fixture::numbers(wavenumber,"Slope",{1.25}); hdf_fixture::numbers(wavenumber,"Intercept",{10});
    }
    constexpr std::size_t vis_rows = 8,vis_cols = 10;
    const hsize_t vis_shape[]{vis_rows,vis_cols},table_shape[]{vis_rows,vis_cols,3},flat_shape[]{vis_rows * vis_cols,3};
    std::vector<float> dn(vis_rows * vis_cols),lat(dn.size()),lon(dn.size()),table(dn.size() * 3);
    for (std::size_t i = 0; i < dn.size(); ++i) {
        const auto row = i / vis_cols;
        dn[i] = float(10 + i % 21); lat[i] = 10 + float(row) * .2f; lon[i] = 140 + float(i % vis_cols) * .2f;
        table[i * 3] = 1e-6f * (1 + i % 3); table[i * 3 + 1] = .001f * (1 + i % 4); table[i * 3 + 2] = 2 + .01f * (i % 5);
    }
    if (masked) { dn[3] = 65535; table[12] = 65535; }
    dataset(file,"Geolocation/Latitude_VIS",vis_shape,lat);
    dataset(file,"Geolocation/Longitude_VIS",vis_shape,lon);
    auto data = dataset(file,"Data/VIS_DN",vis_shape,dn);
    hdf_fixture::numbers(data,"Slope",{.25}); hdf_fixture::numbers(data,"Intercept",{2});
    hdf_fixture::numbers(data,"FillValue",{65535}); hdf_fixture::numbers(data,"valid_range",{0,4096});
    auto calibration = dataset(file,"Data/VIS_CalTable",flat_table ? std::span<const hsize_t>(flat_shape) : table_shape,table);
    hdf_fixture::numbers(calibration,"Slope",{.5}); hdf_fixture::numbers(calibration,"Intercept",{.0001});
    hdf_fixture::numbers(calibration,"FillValue",{65535});
    hdf_fixture::numbers(calibration,"valid_range",{-1,1});
}

}
