/// SPDX-License-Identifier: MIT

#include "legacy_bridge.hpp"

#include <bit>
#include <fstream>
#include <iostream>

template<class T>
void write_plane(const std::filesystem::path& path, const std::vector<T>& values) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(reinterpret_cast<const char*>(values.data()),
                 static_cast<std::streamsize>(values.size() * sizeof(T)));
}

int main(int argc, char** argv) {
    if (argc != 6) return 1;
    try {
        fy::Request request;
        request.mode = argv[1];
        request.band = std::stoi(argv[2]);
        request.inputs.push_back({argv[3], argv[4], {}});
        auto scene = fy::read_mersi(request.inputs.front(), request);
        const std::filesystem::path directory(argv[5]);
        std::filesystem::create_directories(directory);
        const auto& frame = scene.image;
        write_plane(directory / "values.f32", frame.values);
        write_plane(directory / "latitude.f32", frame.geo.latitude.values);
        write_plane(directory / "longitude.f32", frame.geo.longitude.values);
        write_plane(directory / "valid.u8", frame.valid.values);
        write_plane(directory / "color.bgr8", frame.color.values);
        cv::FileStorage metadata((directory / "frame.json").string(),
                                 cv::FileStorage::WRITE | cv::FileStorage::FORMAT_JSON);
        metadata << "rows" << frame.color.rows << "cols" << frame.color.cols;
        metadata << "value_count" << static_cast<double>(frame.values.size());
        metadata << "low" << frame.scale.lowval << "high" << frame.scale.hival;
        metadata << "has_colorbar" << frame.has_colorbar;
        metadata << "endian" << (std::endian::native == std::endian::little ? "little" : "big");
        metadata << "satellite" << scene.satellite << "sensor" << scene.sensor;
        metadata << "labels" << scene.labels << "extra_label" << scene.extra_label;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
