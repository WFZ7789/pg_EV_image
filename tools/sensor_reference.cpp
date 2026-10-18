/// SPDX-License-Identifier: MIT

#include "core/navigation.hpp"
#include "core/resample.hpp"
#include "core/stream.hpp"
#include "sensors/hdf.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 7) return 1;
    try {
        fy::core::ExecutionContext context({});
        fy::core::AdapterRegistry registry;
        fy::sensors::register_hdf_adapters(registry, context.profile());
        auto session = registry.open(argv[1], {argv[2], argv[3], {}});
        auto metadata = session.metadata();
        std::filesystem::path prefix(argv[5]);
        if (prefix.has_parent_path()) std::filesystem::create_directories(prefix.parent_path());
        if (std::string(argv[6]) == "geometry") {
            const auto& target_grid = metadata.grid(argv[4]);
            const auto source_id = target_grid.paired_grid.empty() ? target_grid.id : target_grid.paired_grid;
            const auto source_shape = metadata.grid(source_id).shape;
            const auto target_shape = target_grid.shape;
            std::ofstream latitude(prefix.string() + ".latitude.f32", std::ios::binary);
            std::ofstream longitude(prefix.string() + ".longitude.f32", std::ios::binary);
            std::ofstream valid(prefix.string() + ".u8", std::ios::binary);
            for (auto* output : {&latitude, &longitude, &valid}) output->exceptions(std::ios::badbit | std::ios::failbit);
            for (std::size_t row = 0; row < target_shape.rows; row += 256) {
                const fy::core::Window output{row, 0, {std::min<std::size_t>(256, target_shape.rows - row), target_shape.cols}};
                const auto input = fy::core::cubic_source_window(source_shape, target_shape, output);
                auto native = session.read_geometry(source_id, input);
                auto geometry = [&]() -> fy::core::SwathGeometry {
                    if (std::holds_alternative<fy::core::SwathGeometry>(native))
                        return fy::core::resample_geolocation(std::get<fy::core::SwathGeometry>(native),input,
                                                             source_shape,output,target_shape,context);
                    if (source_shape != target_shape) throw std::invalid_argument("Disk reference grid mismatch");
                    const auto& disk = std::get<fy::core::NativeDiskGeometry>(native);
                    fy::core::SwathGeometry result{fy::core::Raster<float>(output.shape),fy::core::Raster<float>(output.shape),
                                                   fy::core::Raster<std::uint8_t>(output.shape)};
                    context.parallel_rows(output.shape.rows,[&](std::size_t begin,std::size_t end) {
                        for (std::size_t row = begin; row < end; ++row)
                            for (std::size_t col = 0; col < output.shape.cols; ++col) {
                                const auto position = fy::core::disk_geographic_at(disk,row + output.row - input.row,col + output.col - input.col);
                                result.latitude.view().at(row,col) = float(position.latitude);
                                result.longitude.view().at(row,col) = float(position.longitude);
                                result.valid.view().at(row,col) = position.valid;
                            }
                    });
                    context.profile().record_computed(output.shape.elements());
                    return result;
                }();
                latitude.write(reinterpret_cast<const char*>(geometry.latitude.values().data()), geometry.latitude.bytes());
                longitude.write(reinterpret_cast<const char*>(geometry.longitude.values().data()), geometry.longitude.bytes());
                valid.write(reinterpret_cast<const char*>(geometry.valid.values().data()), geometry.valid.bytes());
            }
            latitude.close(); longitude.close(); valid.close();
            context.profile().set_source_pixels(target_shape.elements());
            context.profile().finish(true);
            context.profile().write_json(prefix.string() + ".profile.json");
            std::cout << "GEO " << target_shape.rows << 'x' << target_shape.cols << '\n';
            return 0;
        }
        const int id = std::stoi(argv[4]);
        const auto bands = session.bands();
        const auto found = std::ranges::find(bands, id, &fy::core::BandDescriptor::id);
        if (found == bands.end()) throw std::invalid_argument("Requested reference band is missing");
        const auto shape = metadata.grid(found->grid).shape;
        std::ofstream values(prefix.string() + ".f32", std::ios::binary);
        std::ofstream valid(prefix.string() + ".u8", std::ios::binary);
        values.exceptions(std::ios::badbit | std::ios::failbit);
        valid.exceptions(std::ios::badbit | std::ios::failbit);
        const bool celsius = std::string(argv[6]) == "celsius";
        fy::core::stream_calibrated(session, id, {0, 0, shape}, context, [&](fy::core::SampleTile&& tile) {
            auto& block = tile.samples;
            if (celsius)
                for (auto& value : block.values.values()) value += -273.15;
            values.write(reinterpret_cast<const char*>(block.values.values().data()), block.values.bytes());
            valid.write(reinterpret_cast<const char*>(block.valid.values().data()), block.valid.bytes());
        });
        values.close(); valid.close();
        context.profile().set_source_pixels(shape.elements());
        context.profile().finish(true);
        context.profile().write_json(prefix.string() + ".profile.json");
        std::cout << metadata.satellite << ' ' << metadata.sensor << ' ' << shape.rows << 'x' << shape.cols << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "HDF reference export failed\n";
        return 2;
    }
}
