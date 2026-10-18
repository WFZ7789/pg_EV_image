/// SPDX-License-Identifier: MIT

#include "config.h"
#include "read.h"

#include <fstream>

int main(int argc, char** argv) {
    if (argc != 6) return 1;
    try {
        H5::H5File file(argv[2], H5F_ACC_RDONLY);
        std::vector<float> values;
        if (std::string(argv[1]) == "mwri") {
            auto dataset = file.openDataSet(argv[3]);
            values = rer_f32_pst_bdid_base(dataset, std::stoul(argv[4])).first;
        } else if (std::string(argv[1]) == "fy4") {
            values = read_fy4(file,std::stoi(argv[4])).first.first;
        } else if (std::string(argv[1]) == "fy4-lat" || std::string(argv[1]) == "fy4-lon") {
            auto shape = read_dims(file.openDataSet(argv[3])).first;
            auto geometry = read_fy4_latlon(file,static_cast<int>(shape.at(0)),static_cast<int>(shape.at(1)));
            values = std::string(argv[1]) == "fy4-lat" ? std::move(geometry.first) : std::move(geometry.second);
        } else {
            auto root = file.openGroup("/");
            const auto satellite = read_str_attr(root.openAttribute("Satellite Name"));
            const auto alias = read_str_attr(root.openAttribute("File Alias Name"));
            const auto& band = kSatConfigs.at(satellite).bands.at(std::stoi(argv[4]));
            const bool quarter = alias.find("250M") != std::string::npos || alias.find("QKM") != std::string::npos;
            if (std::string(argv[1]) == "mersi-e") {
                values = read_emissive(file, band, satellite, quarter).first.first;
                for (auto& value : values) value += K2c;
            } else values = read_reflectance_configured(file, band, get_viscal_name(satellite), quarter).first.first;
        }
        std::ofstream output(argv[5], std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.write(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float));
        return 0;
    } catch (...) { return 2; }
}
