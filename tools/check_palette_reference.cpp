/// SPDX-License-Identifier: MIT

#include "products/palette.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
    if (argc != 4) return 1;
    try {
        static_assert(std::endian::native == std::endian::little);
        const std::filesystem::path directory(argv[1]);
        const RenderConfig scale{std::stof(argv[2]),std::stof(argv[3])};
        if (!std::isfinite(scale.lowval) || !std::isfinite(scale.hival) || scale.lowval >= scale.hival)
            throw std::invalid_argument("Invalid frozen display scale");
        const auto bytes = std::filesystem::file_size(directory / "values.f32");
        const auto samples = bytes / sizeof(float);
        if (!samples || bytes % sizeof(float) || std::filesystem::file_size(directory / "color.bgr8") != samples * 3)
            throw std::invalid_argument("Frozen frame size mismatch");
        std::ifstream values(directory / "values.f32",std::ios::binary),colors(directory / "color.bgr8",std::ios::binary);
        values.exceptions(std::ios::badbit | std::ios::failbit); colors.exceptions(std::ios::badbit | std::ios::failbit);
        std::array<float,4096> block;
        std::array<std::uint8_t,4096 * 3> reference;
        int maximum = 0;
        std::uintmax_t changed = 0;
        for (std::uintmax_t start = 0; start < samples; start += block.size()) {
            const auto count = std::min<std::uintmax_t>(block.size(),samples - start);
            values.read(reinterpret_cast<char*>(block.data()),static_cast<std::streamsize>(count * sizeof(float)));
            colors.read(reinterpret_cast<char*>(reference.data()),static_cast<std::streamsize>(count * 3));
            for (std::size_t i = 0; i < count; ++i) {
                const auto color = fy::products::palette::color_rbtop_wv(block[i],scale);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const auto difference = std::abs(int(color[channel]) - reference[i * 3 + channel]);
                    maximum = std::max(maximum,difference); changed += difference != 0;
                }
            }
        }
        std::cout << "{\"samples\":" << samples << ",\"maximum_channel_difference\":" << maximum
                  << ",\"changed_channels\":" << changed << "}\n";
        return maximum ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
