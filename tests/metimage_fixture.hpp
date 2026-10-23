/// SPDX-License-Identifier: MIT

#pragma once

#include "hdf_fixture.hpp"
#include "sensors/metimage.hpp"

#include <cmath>

namespace metimage_fixture {

inline double radiance(double wavelength_um,double temperature) {
    constexpr double h = 6.626069e-34,c = 299792458,k = 1.38065e-23;
    const double wavelength = wavelength_um*1e-6;
    return 2*h*c*c/(std::pow(wavelength,5)*std::expm1(h*c/(k*wavelength*temperature)))*1e-6;
}

inline H5::DataSet packed(H5::H5File& file,const std::string& path,std::span<const hsize_t> dims,
                         std::span<const float> values,double scale = 1,double offset = 0,
                         double low = -32767,double high = 32767,double fill = -32768) {
    auto data = hdf_fixture::dataset(file,path,dims,values);
    hdf_fixture::numbers(data,"scale_factor",{scale}); hdf_fixture::numbers(data,"add_offset",{offset});
    hdf_fixture::numbers(data,"valid_min",{low}); hdf_fixture::numbers(data,"valid_max",{high});
    hdf_fixture::numbers(data,"_FillValue",{fill});
    return data;
}

inline void create(const std::filesystem::path& path,bool night = false,int segment = 0) {
    H5::H5File file(path.string(),H5F_ACC_TRUNC);
    auto root = file.openGroup("/");
    for (const auto& [key,value] : {std::pair{"spacecraft","SGA1"},{"instrument","VII"},
        {"product_level","1B"},{"type","RAD"},{"product_name","Synthetic METimage"}})
        hdf_fixture::text(root,key,value);
    hdf_fixture::text(root,"sensing_start_time_utc","2026-10-22 00:"+std::to_string(56+segment)+":00.000");
    hdf_fixture::text(root,"sensing_end_time_utc","2026-10-22 00:"+std::to_string(57+segment)+":00.000");
    hdf_fixture::numbers(root,"orbit_start",{6181});
    file.createGroup("status"); file.createGroup("status/satellite");
    auto distance = file.createDataSet("status/satellite/earth_sun_distance_ratio",H5::PredType::NATIVE_DOUBLE,H5::DataSpace(H5S_SCALAR));
    const double ratio = 1.01+segment*.001; distance.write(&ratio,H5::PredType::NATIVE_DOUBLE);
    const hsize_t dims[]{48,16},tie_dims[]{8,3},scan_band[]{2,20},scans[]{2},dup_dims[]{24,16},perf_dims[]{20,24};
    for (int id = 0; id < 20; ++id) {
        const auto name = std::string(fy::sensors::metimage_channels[id]);
        std::vector<float> values(48*16);
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = night && id < 11 ? 8191 : 1000+float(i/16)/5+float(i%16)/5+segment*2;
        if (!night && id == 0) { values[0] = 8191; values[1] = 8190; values[2] = 50000; }
        const double scale = id < 11 ? .02 : radiance(std::stod(name)/1000.,292)/1000;
        packed(file,"data/measurement_data/vii_"+name,dims,values,scale,id < 11 ? -.1 : 0,0,8189,8191);
    }
    for (const auto& name : {"latitude","longitude","solar_zenith","solar_azimuth","observation_zenith","observation_azimuth"}) {
        std::vector<float> values(8*3);
        for (std::size_t row = 0; row < 8; ++row)
            for (std::size_t col = 0; col < 3; ++col) {
                double value = 0;
                const std::string field(name);
                if (field == "latitude") value = 10+segment*.2+row/4*.1+row%4*.08;
                if (field == "longitude") value = 140+col*.08+row/4*.01+segment*.02;
                if (field == "solar_zenith") value = night ? 140 : 45;
                if (field == "solar_azimuth") value = col ? 1+2*(col-1) : 359;
                if (field == "observation_zenith") value = 20;
                if (field == "observation_azimuth") value = 90;
                values[row*3+col] = float(std::round(value*1000));
            }
        packed(file,"data/measurement_data/"+std::string(name),tie_dims,values,.001,0,-90000,360000,-999999);
    }
    for (const auto& [name,value] : {std::pair{"delta_lat_N_dem",100.f},{"delta_lon_E_dem",50.f}})
        packed(file,"data/measurement_data/"+std::string(name),dims,std::vector<float>(48*16,value));
    std::vector<float> irradiance(11),wavelengths(9),a(9,1.002),b(9,-.15);
    for (std::size_t i = 0; i < 11; ++i) irradiance[i] = float(1700+i*10+segment);
    for (std::size_t i = 0; i < 9; ++i) wavelengths[i] = std::stof(std::string(fy::sensors::metimage_channels[i+11]))/1000;
    const hsize_t solar_dims[]{11},thermal_dims[]{9};
    hdf_fixture::dataset(file,"data/calibration_data/band_averaged_solar_irradiance",solar_dims,irradiance);
    hdf_fixture::dataset(file,"data/calibration_data/channel_cw_thermal",thermal_dims,wavelengths);
    hdf_fixture::dataset(file,"data/calibration_data/bt_conversion_a",thermal_dims,a);
    hdf_fixture::dataset(file,"data/calibration_data/bt_conversion_b",thermal_dims,b);
    std::vector<float> data(40),cal(40),proc(40),geo(2),dup(24*16),perf(20*24);
    if (night) {
        for (std::size_t scan = 0; scan < 2; ++scan)
            for (std::size_t band = 0; band < 11; ++band) data[scan*20+band] = proc[scan*20+band] = 1;
    } else {
        data[2] = 1; data[3] = 2; data[24] = 4;
        cal[5] = 8; cal[6] = 16; proc[7] = 2; proc[8] = 4;
    }
    dup[2*16+4] = 1; dup[2*16+5] = 2; perf[1*24+3] = 1;
    hdf_fixture::dataset(file,"data/quality_information/data_quality",scan_band,data,H5::PredType::NATIVE_UINT8);
    hdf_fixture::dataset(file,"data/quality_information/calibration_quality",scan_band,cal,H5::PredType::NATIVE_UINT16);
    hdf_fixture::dataset(file,"data/quality_information/geolocation_quality",scans,geo,H5::PredType::NATIVE_UINT8);
    hdf_fixture::dataset(file,"data/processing_flags/vii_processing_flag",scan_band,proc,H5::PredType::NATIVE_UINT8);
    hdf_fixture::dataset(file,"data/processing_flags/pixel_duplication_mask",dup_dims,dup,H5::PredType::NATIVE_UINT8);
    hdf_fixture::dataset(file,"data/processing_flags/pixel_performance_mask",perf_dims,perf,H5::PredType::NATIVE_UINT8);
}

}
