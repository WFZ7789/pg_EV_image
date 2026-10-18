/// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace product_reference {

struct Frame {
    std::size_t rows = 0,cols = 0;
    int depth = 8;
    struct { float lowval = 0,hival = 1; } scale;
    std::vector<std::string> labels;
    std::vector<float> values,lat,lon,wavenumbers;
    std::vector<std::array<std::uint16_t,3>> pixels;
};

inline Frame read(const std::string& name) {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "reference_v1" / (name + ".bin");
    std::ifstream input(path,std::ios::binary);
    input.exceptions(std::ios::badbit | std::ios::failbit);
    auto integer = [&](int count) {
        std::uint32_t value = 0;
        for (int byte = 0; byte < count; ++byte) value |= std::uint32_t(static_cast<unsigned char>(input.get())) << (byte * 8);
        return value;
    };
    const auto real = [&] { return std::bit_cast<float>(integer(4)); };
    std::array<char,8> magic{};
    input.read(magic.data(),magic.size());
    if (magic != std::array{'F','Y','R','E','F','0','0','1'}) throw std::runtime_error("Invalid product reference: " + name);
    Frame frame;
    frame.rows = integer(4); frame.cols = integer(4); frame.depth = static_cast<int>(integer(4));
    frame.scale.lowval = real(); frame.scale.hival = real();
    const auto labels = integer(4);
    if (labels > 100) throw std::runtime_error("Invalid reference label count");
    for (std::uint32_t index = 0; index < labels; ++index) {
        const auto size = integer(4);
        if (size > 65536) throw std::runtime_error("Invalid reference label size");
        std::string label(size,'\0'); input.read(label.data(),size); frame.labels.push_back(std::move(label));
    }
    for (auto* plane : {&frame.values,&frame.lat,&frame.lon,&frame.wavenumbers}) {
        const auto count = integer(4);
        if (count > 1000000) throw std::runtime_error("Invalid reference plane size");
        plane->resize(count);
        for (auto& value : *plane) value = real();
    }
    const auto pixels = integer(4);
    if (pixels > 1000000) throw std::runtime_error("Invalid reference image size");
    frame.pixels.resize(pixels);
    for (auto& pixel : frame.pixels)
        for (auto& channel : pixel) channel = static_cast<std::uint16_t>(integer(2));
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing reference bytes");
    return frame;
}

inline Frame next(const std::string& group) {
    static std::map<std::string,std::size_t> counters;
    return read(group + "-" + std::to_string(counters[group]++));
}

}
