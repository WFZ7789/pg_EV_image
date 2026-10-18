/// SPDX-License-Identifier: MIT

#pragma once

#include <H5Cpp.h>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace hdf_fixture {

inline void text(H5::H5Object& object, const std::string& name, const std::string& value) {
    H5::StrType type(H5::PredType::C_S1, value.size() + 1);
    auto attribute = object.createAttribute(name, type, H5::DataSpace(H5S_SCALAR));
    attribute.write(type, value);
}

inline void numbers(H5::H5Object& object, const std::string& name, std::initializer_list<double> values) {
    const hsize_t count = values.size();
    auto attribute = object.createAttribute(name, H5::PredType::NATIVE_DOUBLE, H5::DataSpace(1, &count));
    attribute.write(H5::PredType::NATIVE_DOUBLE, values.begin());
}

inline void metadata(H5::H5File& file, const std::string& satellite, const std::string& sensor,
                      const std::string& alias = "1000M") {
    auto root = file.openGroup("/");
    text(root, "Satellite Name", satellite);
    text(root, "Sensor Identification Code", sensor);
    text(root, "File Alias Name", alias);
    text(root, "File Name", "synthetic.HDF");
    text(root, "Observing Beginning Date", "2026-06-18");
    text(root, "Observing Beginning Time", "00:50:00.000");
}

inline H5::DataSet dataset(H5::H5File& file, const std::string& path,
                           std::span<const hsize_t> dimensions, std::span<const float> values,
                           const H5::DataType& storage = H5::PredType::NATIVE_FLOAT) {
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        const auto group = path.substr(0, slash);
        if (H5Lexists(file.getId(), group.c_str(), H5P_DEFAULT) <= 0) file.createGroup(group);
    }
    auto result = file.createDataSet(path, storage,
                                      H5::DataSpace(static_cast<int>(dimensions.size()), dimensions.data()));
    result.write(values.data(), H5::PredType::NATIVE_FLOAT);
    text(result, "long_name", "Synthetic data");
    return result;
}

inline void geolocation(H5::H5File& file, const std::string& prefix, std::size_t rows, std::size_t cols,
                         bool invalid = false) {
    const hsize_t dimensions[]{rows, cols};
    std::vector<float> lat(rows * cols), lon(rows * cols);
    for (std::size_t i = 0; i < lat.size(); ++i) {
        lat[i] = static_cast<float>(10 + i / cols);
        lon[i] = static_cast<float>(140 + i % cols);
    }
    if (invalid) lat.at(1) = -9999.9f;
    auto latitude = dataset(file, prefix + "/Latitude", dimensions, lat);
    auto longitude = dataset(file, prefix + "/Longitude", dimensions, lon);
    numbers(latitude, "FillValue", {-9999.9});
    numbers(longitude, "FillValue", {-9999.9});
}

}
