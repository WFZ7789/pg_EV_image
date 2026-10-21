/// SPDX-License-Identifier: MIT

#include <H5Cpp.h>
#include "fy4_fixture.hpp"
#include "pmr_fixture.hpp"
#include "giirs_fixture.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

void text_attribute(H5::H5Object& object, const char* name, const std::string& value) {
    H5::StrType type(H5::PredType::C_S1, value.size() + 1);
    auto attribute = object.createAttribute(name, type, H5::DataSpace(H5S_SCALAR));
    attribute.write(type, value);
}

void scalar_attribute(H5::H5Object& object, const char* name, float value) {
    auto attribute = object.createAttribute(name, H5::PredType::NATIVE_FLOAT, H5::DataSpace(H5S_SCALAR));
    attribute.write(H5::PredType::NATIVE_FLOAT, &value);
}

void scene(const std::filesystem::path& path, const std::string& time, float shift,bool saturated) {
    constexpr std::uint16_t rows = 32, cols = 64;
    H5::H5File file(path.string(), H5F_ACC_TRUNC);
    auto root = file.openGroup("/");
    text_attribute(root, "Satellite Name", "FY-3H");
    text_attribute(root, "Sensor Identification Code", "MERSI");
    text_attribute(root, "File Alias Name", "MERSI_1000M");
    text_attribute(root, "File Name", path.filename().string());
    text_attribute(root, "Observing Beginning Date", "2026-06-18");
    text_attribute(root, "Observing Beginning Time", time);
    scalar_attribute(root,"EarthSun Distance Ratio",1);
    for (auto [name, value] : {std::pair{"Scan_Line_number", rows}, {"Pixels_per_Scan", cols}}) {
        auto attribute = root.createAttribute(name, H5::PredType::NATIVE_UINT16, H5::DataSpace(H5S_SCALAR));
        attribute.write(H5::PredType::NATIVE_UINT16, &value);
    }
    hsize_t coefficient_count = 12;
    std::vector<float> coefficients(12, 0);
    for (std::size_t i = 0; i < 6; ++i) coefficients[i] = 1;
    auto coefficient = root.createAttribute("TBB_Trans_Coefficient", H5::PredType::NATIVE_FLOAT,
                                            H5::DataSpace(1, &coefficient_count));
    coefficient.write(H5::PredType::NATIVE_FLOAT, coefficients.data());
    file.createGroup("Data");
    file.createGroup("Geolocation");
    hsize_t dimensions[]{1, rows, cols};
    auto radiance = file.createDataSet("Data/EV_250_Aggr.1KM_Emissive", H5::PredType::NATIVE_FLOAT,
                                       H5::DataSpace(3, dimensions));
    std::vector<float> values(rows * cols), latitude(rows * cols), longitude(rows * cols);
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t col = 0; col < cols; ++col) {
            auto i = row * cols + col;
            values[i] = 30 + static_cast<float>((row + col) % 50);
            latitude[i] = 10 + static_cast<float>(row) * 0.2f + shift;
            longitude[i] = 140 + static_cast<float>(col) * 0.2f;
        }
    radiance.write(values.data(), H5::PredType::NATIVE_FLOAT);
    scalar_attribute(radiance, "Slope", 1);
    scalar_attribute(radiance, "Intercept", 0);
    text_attribute(radiance, "long_name", "Synthetic emissive channel");
    for (auto [name, data] : {std::pair{"Geolocation/Latitude", latitude.data()},
                             {"Geolocation/Longitude", longitude.data()}}) {
        auto dataset = file.createDataSet(name, H5::PredType::NATIVE_FLOAT, H5::DataSpace(2, dimensions + 1));
        dataset.write(data, H5::PredType::NATIVE_FLOAT);
    }
    for (const auto name : {"SolarZenith","SolarAzimuth","SensorZenith","SensorAzimuth"}) {
        auto dataset = file.createDataSet(std::string("Geolocation/") + name,H5::PredType::NATIVE_FLOAT,H5::DataSpace(2,dimensions + 1));
        std::vector<float> angles(rows * cols,name == std::string("SolarZenith") ? 3000 : 0);
        dataset.write(angles.data(),H5::PredType::NATIVE_FLOAT);
        scalar_attribute(dataset,"Slope",.01f); scalar_attribute(dataset,"Intercept",0);
    }
    file.createGroup("Calibration");
    const hsize_t coefficient_dims[]{19,3};
    std::vector<float> visible_coefficients(19 * 3);
    for (int channel = 0; channel < 19; ++channel) visible_coefficients[channel * 3 + 1] = 1;
    auto visible = file.createDataSet("Calibration/VIS_Cal_Coeff",H5::PredType::NATIVE_FLOAT,H5::DataSpace(2,coefficient_dims));
    visible.write(visible_coefficients.data(),H5::PredType::NATIVE_FLOAT);
    for (auto [name,channels] : {std::pair{"Data/EV_250_Aggr.1KM_RefSB",4}, {"Data/EV_1KM_RefSB",15}}) {
        const hsize_t dims[]{static_cast<hsize_t>(channels),rows,cols};
        std::vector<float> reflection(static_cast<std::size_t>(channels) * rows * cols);
        for (int channel = 0; channel < channels; ++channel)
            for (std::size_t row = 0; row < rows; ++row)
                for (std::size_t col = 0; col < cols; ++col)
                    reflection[(channel * rows + row) * cols + col] = saturated ? 1200 : float(10 + (row * 3 + col * 5 + channel * 7) % 80) + shift;
        auto dataset = file.createDataSet(name,H5::PredType::NATIVE_FLOAT,H5::DataSpace(3,dims));
        dataset.write(reflection.data(),H5::PredType::NATIVE_FLOAT);
        scalar_attribute(dataset,"Slope",1); scalar_attribute(dataset,"Intercept",0);
        text_attribute(dataset,"long_name","Synthetic reflection channel");
    }
}

}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 1;
    const bool saturated = argc == 3 && std::string(argv[2]) == "saturated";
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    if (argc == 3 && std::string(argv[2]) == "giirs") {
        giirs_fixture::write(directory / "giirs.HDF");
        giirs_fixture::write(directory / "giirs-masked.HDF",true);
        giirs_fixture::write(directory / "giirs-empty-channel.HDF",false,true);
        giirs_fixture::write(directory / "giirs-empty-family.HDF",false,false,true);
        return 0;
    }
    if (argc == 3 && std::string(argv[2]) == "pmr") {
        pmr_fixture::write(directory / "pmr.HDF");
        pmr_fixture::write(directory / "pmr-masked.HDF",true);
        pmr_fixture::write(directory / "pmr-data.HDF",false,false,false);
        pmr_fixture::write(directory / "pmr-geo.HDF",false,false,true,false);
        pmr_fixture::write(directory / "pmr-explicit.HDF",false,true);
        return 0;
    }
    if (argc == 3 && std::string(argv[2]) == "fy4") {
        fy4_fixture::write(directory / "fy4.HDF","FY-4B","AGRI",256,256,true,true);
        fy4_fixture::write(directory / "fy4-aux.HDF","FY-4B","AGRI",128,128,false);
        fy4_fixture::write(directory / "ghi.HDF","FY-4B","GHI",192,256,false);
        fy4_fixture::write(directory / "ghi-aux.HDF","FY-4B","GHI",384,512,false);
        return 0;
    }
    scene(directory / "FY3H_MERSI_20260618_0050_1000M_V0.HDF", "00:50:00.000", 0,saturated);
    scene(directory / "FY3H_MERSI_20260618_0055_1000M_V0.HDF", "00:55:00.000", 3,saturated);
}
