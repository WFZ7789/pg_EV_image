/// SPDX-License-Identifier: MIT

#pragma once

#include "hdf_fixture.hpp"
#include "sensors/amsr3.hpp"

#include <set>

namespace amsr3_fixture {

inline void create(const std::filesystem::path& path) {
    H5::H5File file(path.string(),H5F_ACC_TRUNC);
    auto root = file.openGroup("/");
    for (auto [key,value] : {std::pair{"PlatformShortName","GOSAT-GW"},{"SensorShortName","AMSR3"},
         {"processing_level","Level1B"},{"ProductName","AMSR3 L1B TBB"},
         {"time_coverage_start","2026-10-20T03:33:36.591Z"},{"time_coverage_end","2026-10-20T04:22:45.794Z"}})
        hdf_fixture::text(root,key,value);
    std::set<std::string> grids;
    constexpr std::size_t rows = 6;
    for (std::size_t id = 0; id < fy::sensors::amsr3_channels.size(); ++id) {
        const auto [channel,g] = fy::sensors::amsr3_channels[id];
        const std::string grid(g),name = "Tb_Ch"+std::string(channel);
        const bool high = grid.starts_with("89");
        const std::size_t cols = high ? 16 : 8;
        const hsize_t dims[]{rows,cols};
        std::vector<float> raw(rows*cols),quality(rows*cols);
        for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = 22000+float(id)*100+float(i/cols)*20+float(i%cols)*5;
        if (id == 0) {
            raw[0] = 65535; raw[1] = 50001;
            quality[2] = 1; quality[3] = 2; quality[4] = 4;
            quality[5] = 8; quality[6] = 128; quality[7] = 255;
        }
        auto data = hdf_fixture::dataset(file,name,dims,raw,H5::PredType::NATIVE_UINT16);
        hdf_fixture::text(data,"units","K");
        hdf_fixture::numbers(data,"scale_factor",{.01}); hdf_fixture::numbers(data,"add_offset",{0});
        hdf_fixture::numbers(data,"valid_min",{0}); hdf_fixture::numbers(data,"valid_max",{50000});
        hdf_fixture::numbers(data,"_FillValue",{65535});
        hdf_fixture::dataset(file,name+"_Quality",dims,quality,H5::PredType::NATIVE_UINT8);
        if (grids.insert(grid).second) {
            std::vector<float> lat(rows*cols),lon(rows*cols);
            for (std::size_t i = 0; i < lat.size(); ++i) {
                const float shift = grid == "89B" ? -.07f : high ? .02f : float(id)*.0005f;
                const float x = float(i%cols)*(high ? .5f : 1.f)-3.5f;
                lat[i] = 10+float(i/cols)*.2f+shift+.02f*x*x;
                lon[i] = 140+float(i%cols)*(high ? .1f : .2f);
            }
            if (id == 18) lat[0] = -9999;
            for (auto [prefix,values] : {std::pair{"Latitude_P",&lat},{"Longitude_P",&lon}}) {
                auto coordinate = hdf_fixture::dataset(file,prefix+grid,dims,*values);
                hdf_fixture::numbers(coordinate,"_FillValue",{-9999});
                hdf_fixture::numbers(coordinate,"scale_factor",{1}); hdf_fixture::numbers(coordinate,"add_offset",{0});
            }
        }
    }
    const hsize_t scans[]{rows};
    std::vector<float> quality(rows); quality.back() = 16;
    hdf_fixture::dataset(file,"ScanDataQuality",scans,quality,H5::PredType::NATIVE_UINT8);
}

}
