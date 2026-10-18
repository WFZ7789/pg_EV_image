/// SPDX-License-Identifier: MIT

#include "core/calibration.hpp"
#include "core/navigation.hpp"
#include "sensors/hdf.hpp"
#include "hdf_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <source_location>

namespace {

using namespace fy::core;

void require(bool condition,std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("FY4 adapter check failed at line " + std::to_string(location.line()));
}

template<class F> void rejects(F&& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected FY4 adapter rejection");
}

void make_file(const std::filesystem::path& path,const std::string& sensor,std::size_t size,
               bool navigation = true) {
    using namespace hdf_fixture;
    H5::H5File file(path.string(),H5F_ACC_TRUNC);
    metadata(file,"FY-4B",sensor,size == 8 ? "1000M" : "4000M");
    numbers(file,"Begin Line Number",{0}); numbers(file,"Begin Pixel Number",{0});
    numbers(file,"End Line Number",{double(size - 1)}); numbers(file,"End Pixel Number",{double(size - 1)});
    numbers(file,sensor == "GHI" ? "Earth_Sun Distance Ratio" : "Earth/Sun Distance Ratio",{1.012});
    if (navigation) {
        numbers(file,"NOMCenterLat",{0}); numbers(file,"NOMCenterLon",{105});
        numbers(file,"NOMSatHeight",{35786000}); numbers(file,"dEA",{6377830});
        numbers(file,"dObRecFlat",{298.257223563});
        numbers(file,"dSamplingAngle",{1000}); numbers(file,"dSteppingAngle",{1100});
    }
    const hsize_t shape[]{size,size}, table_shape[]{8};
    for (const int channel : {1,13}) {
        const auto suffix = channel == 1 ? "01" : "13";
        std::vector<float> raw(size * size),table(8);
        for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = float(i % 8);
        raw[0] = 65535;
        for (std::size_t i = 0; i < table.size(); ++i) table[i] = channel == 1 ? float(i) * 0.125f : 230 + float(i);
        table[5] = -65535;
        table[6] = NAN;
        auto data = dataset(file,std::string("Data/NOMChannel") + suffix,shape,raw,H5::PredType::NATIVE_UINT16);
        numbers(data,"FillValue",{65535}); numbers(data,"valid_range",{0,7});
        auto calibration = dataset(file,std::string("Calibration/CALChannel") + suffix,table_shape,table);
        numbers(calibration,"FillValue",{-65535});
    }
    const hsize_t angles_shape[]{4,4};
    for (auto name : {"NOMSunZenith","NOMSunAzimuth","NOMSatelliteZenith"}) {
        std::vector<float> angles(16,30);
        angles[0] = 65535;
        auto data = dataset(file,std::string("Navigation/") + name,angles_shape,angles);
        numbers(data,"FillValue",{65535}); numbers(data,"Slope",{0.5}); numbers(data,"Intercept",{2});
    }
}

}

int main() {
    ExecutionContext context({.threads = 4});
    context.io().run([] { static_cast<void>(H5::PredType::NATIVE_FLOAT.getSize()); });
    const auto initial = context.io().run([] { return H5Fget_obj_count(H5F_OBJ_ALL,H5F_OBJ_ALL); });
    AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry,context.profile());
    const auto directory = context.scratch().path();
    const auto primary = directory / "primary.HDF", auxiliary = directory / "auxiliary.HDF";
    context.io().run([&] { make_file(primary,"AGRI",8); make_file(auxiliary,"AGRI",4); });
    {
        auto session = registry.open("fy4",{primary,primary,auxiliary});
        const auto metadata = session.metadata();
        require(metadata.satellite == "FY-4B" && metadata.sensor == "AGRI" && metadata.resolution == "1000M");
        require(metadata.numeric_attributes.at("earth_sun_distance_au") == 1.012);
        require(metadata.grid("native").shape == Shape{8,8} && metadata.grid("auxiliary").shape == Shape{4,4});
        require(metadata.grid("angles").shape == Shape{4,4});
        const auto bands = session.bands();
        auto band = [&](int id) -> const BandDescriptor& {
            auto found = std::ranges::find(bands,id,&BandDescriptor::id);
            require(found != bands.end());
            return *found;
        };
        require(band(1).unit == "DN" && band(1).calibrated_unit == "1" && band(13).calibrated_unit == "K");
        require(band(1013).source == SourceRole::auxiliary && band(1013).calibration.source == SourceRole::auxiliary);
        require(band(13).calibration.kind == CalibrationKind::lookup && band(13).calibration.coefficients.size() == 8);
        const auto before = context.profile().snapshot().io.value();
        auto block = session.read_window(13,{1,2,{2,3}});
        const auto after = context.profile().snapshot().io.value();
        require(after.read_calls - before.read_calls == 1 && after.read_samples - before.read_samples == 6);
        require(after.logical_bytes - before.logical_bytes == 12);
        calibrate(block,band(13).calibration,context);
        require(block.values.view().at(1,2) == 234 && block.valid.view().at(1,2));
        auto invalid = session.read_window(13,{0,0,{1,8}});
        calibrate(invalid,band(13).calibration,context);
        for (std::size_t i : {0u,5u,6u}) require(!invalid.valid.values()[i] && std::isnan(invalid.values.values()[i]));
        auto retained = session.read_window(13,{0,0,{1,8}});
        calibrate(retained,band(13).calibration,context,InvalidSamplePolicy::retain_finite);
        require(!retained.valid.values()[5] && retained.values.values()[5] == -65535);
        auto reflected = session.read_window(1,{0,1,{1,2}});
        calibrate(reflected,band(1).calibration,context);
        require(reflected.values.values()[0] == 0.125f && reflected.values.values()[1] == 0.25f);
        auto angle = session.read_window(fy::sensors::solar_zenith,{0,0,{2,2}});
        calibrate(angle,band(fy::sensors::solar_zenith).calibration,context);
        require(!angle.valid.values()[0] && angle.values.values()[1] == 17);
        const auto disk = std::get<NativeDiskGeometry>(session.read_geometry("native",{0,0,{8,8}}));
        require(disk.longitude_deg == 105 && disk.equatorial_radius_m == 6377830 && disk.satellite_distance_m == 42163830);
        require(std::abs(disk.scan_origin_rad[0] + 0.0035) < 1e-15 && std::abs(disk.scan_step_rad[1] + 0.0011) < 1e-15);
        const auto window = std::get<NativeDiskGeometry>(session.read_geometry("native",{2,3,{2,3}}));
        require(window.shape == Shape{2,3});
        require(std::abs(window.scan_origin_rad[0] + 0.0005) < 1e-15 && std::abs(window.scan_origin_rad[1] - 0.00165) < 1e-15);
        const auto center = disk_geographic_at(disk,3.5,3.5);
        require(center.valid && std::abs(center.latitude) < 1e-12 && std::abs(center.longitude - 105) < 1e-12);
        const auto north = disk_geographic_at(disk,1,3.5), east = disk_geographic_at(disk,3.5,6);
        require(north.valid && north.latitude > 0 && east.valid && east.longitude > 105);
        const auto full_position = disk_geographic_at(disk,2.5,3.75), local_position = disk_geographic_at(window,0.5,0.75);
        require(std::abs(full_position.latitude - local_position.latitude) < 1e-12 &&
                std::abs(full_position.longitude - local_position.longitude) < 1e-12);
        require(!disk_geographic_at(disk,3.5,200).valid && !disk_geographic_at(disk,NAN,0).valid);
        rejects([&] { session.read_window(13,{7,7,{2,2}}); });
        rejects([&] { session.read_window(99,{0,0,{1,1}}); });
    }
    {
        auto path = directory / "ghi.HDF";
        context.io().run([&] { make_file(path,"GHI",4,false); });
        auto session = registry.open("fy4",{path,{},{}});
        require(session.metadata().numeric_attributes.at("earth_sun_distance_au") == 1.012);
        const auto block = session.read_window(1,{1,1,{1,1}});
        require(block.values.values()[0] == 5);
        require(std::get<ImageGeometry>(session.read_geometry("native",{0,0,{4,4}})).shape == Shape{4,4});
        rejects([&] { registry.open("fy4",{primary,{},path}); });
    }
    for (const std::string failure : {"time","shape","table","navigation","range"}) {
        auto path = directory / (failure + ".HDF");
        context.io().run([&] {
            make_file(path,"AGRI",4);
            H5::H5File file(path.string(),H5F_ACC_RDWR);
            if (failure == "time") {
                auto attribute = file.openAttribute("Observing Beginning Time");
                attribute.write(attribute.getDataType(),std::string("00:55:00.000"));
            } else if (failure == "shape") {
                auto attribute = file.openAttribute("End Line Number");
                const double value = 2;
                attribute.write(H5::PredType::NATIVE_DOUBLE,&value);
            } else if (failure == "table") H5Ldelete(file.getId(),"Calibration/CALChannel13",H5P_DEFAULT);
            else if (failure == "navigation") {
                auto attribute = file.openAttribute("dObRecFlat");
                const double value = 0;
                attribute.write(H5::PredType::NATIVE_DOUBLE,&value);
            } else {
                auto dataset = file.openDataSet("Data/NOMChannel01");
                auto attribute = dataset.openAttribute("valid_range");
                const double values[]{9,10};
                attribute.write(H5::PredType::NATIVE_DOUBLE,values);
            }
        });
        rejects([&] { registry.open("fy4",{primary,{},path}); });
    }
    const auto open = context.io().run([] { return H5Fget_obj_count(H5F_OBJ_ALL,H5F_OBJ_ALL); });
    require(open == initial);
}
