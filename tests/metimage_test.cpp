/// SPDX-License-Identifier: MIT

#include "metimage_fixture.hpp"
#include "core/calibration.hpp"
#include "products/execute.hpp"
#include "products/registry.hpp"
#include "products/cache.hpp"
#include "sensors/hdf.hpp"
#include "sensors/input.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <source_location>

namespace {

using namespace fy::core;

void require(bool value,std::source_location location = std::source_location::current()) {
    if (!value) throw std::runtime_error("METimage check failed at line "+std::to_string(location.line()));
}

template<class T> void array(std::span<const T> values) {
    std::cout << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) std::cout << ',';
        if (std::isfinite(double(values[i]))) std::cout << double(values[i]);
        else std::cout << "null";
    }
    std::cout << ']';
}

void probe(SensorSession& session,Window window,ExecutionContext& context) {
    const auto bands = session.bands();
    const auto shape = session.metadata().grid("native").shape;
    std::cout << std::setprecision(10) << "{\"shape\":[" << shape.rows << ',' << shape.cols << "],\"bands\":[";
    for (std::size_t i = 0; i < bands.size(); ++i) {
        if (i) std::cout << ',';
        auto block = session.read_window(bands[i].id,window);
        calibrate(block,bands[i].calibration,context);
        std::cout << "{\"id\":" << bands[i].id << ",\"values\":"; array<float>(block.values.values());
        std::cout << ",\"valid\":"; array<std::uint8_t>(block.valid.values()); std::cout << '}';
    }
    auto geometry = std::get<SwathGeometry>(session.read_geometry("native",window));
    std::cout << "],\"latitude\":"; array<float>(geometry.latitude.values());
    std::cout << ",\"longitude\":"; array<float>(geometry.longitude.values());
    std::cout << ",\"geolocation_valid\":"; array<std::uint8_t>(geometry.valid.values()); std::cout << "}\n";
}

void check(const std::filesystem::path& path,ExecutionContext& context,AdapterRegistry& registry) {
    auto session = registry.open("metimage",{path,{},{}});
    const auto metadata = session.metadata(); const auto bands = session.bands();
    require(metadata.sensor == "METimage" && metadata.satellite == "Metop-SG A1" && bands.size() == 24);
    require(metadata.grid("native").shape == Shape{48,16});
    auto raw = session.read_window(0,{0,0,{4,16}});
    require(!raw.valid.values()[0] && !raw.valid.values()[1] && !raw.valid.values()[2]);
    require(!raw.valid.view().at(2,4) && raw.valid.view().at(2,5));
    auto visible = session.read_window(0,{4,4,{1,1}}); calibrate(visible,bands[0].calibration,context);
    require(std::abs(visible.values.values()[0]-(1001.6*.02-.1)*std::numbers::pi*1.01*1.01/1700) < 1e-6);
    const auto valid_at = [&](int id,std::size_t row) {
        const auto block = session.read_window(id,{row,0,{1,1}});
        return bool(block.valid.values()[0]);
    };
    require(!valid_at(1,3) && !valid_at(2,4) && valid_at(3,4) && !valid_at(4,28));
    require(valid_at(5,4) && !valid_at(6,4) && valid_at(7,4) && valid_at(8,4));
    auto thermal = session.read_window(17,{0,0,{1,1}}); calibrate(thermal,bands[17].calibration,context);
    require(std::abs(thermal.values.values()[0]-(292*1.002-.15)) < .002);
    const auto geometry = std::get<SwathGeometry>(session.read_geometry("native",{24,0,{2,16}}));
    require(geometry.scan_rows == 24 && geometry.first_source_row == 24);
    require(std::abs(geometry.latitude.values()[0]-(10.1+100/6371000./(std::numbers::pi/180))) < 2e-5);
    const auto azimuth = session.read_window(fy::sensors::solar_azimuth,{0,4,{1,1}});
    require(std::min(std::abs(azimuth.values.values()[0]),std::abs(azimuth.values.values()[0]-360)) < .001);
    fy::products::ProductRegistry products; fy::products::register_products(products);
    bool unsupported_ek = false;
    try { static_cast<void>(products.plan("metimage",metadata,bands,{.mode = "EK"})); }
    catch (const std::invalid_argument&) { unsupported_ek = true; }
    require(unsupported_ek);
    for (const std::string mode : {"R","RMUL","E","RT","RY","RF","EC","ECT","TSG"}) {
        const auto definition = products.plan("metimage",metadata,bands,{.mode = mode,.band = mode == "E" ? 17 : 0});
        auto loaded = fy::products::load_product(session,definition,context);
        loaded.product.frame.validate();
        require(std::ranges::any_of(loaded.product.frame.planes.front().valid.values(),[](auto v) { return v != 0; }));
        if (mode == "E") {
            fy::products::CachedSwath cached(context.scratch().path()/"scans",loaded,context,24);
            const MapView map{{false,0,140},{139.9,140.3,9.9,10.5},{80,80}};
            cached.project(map);
            const auto sources = cached.sources(0);
            require(sources.size() == 2 && sources[0].cells == Shape{24,16});
            const auto block = sources[1].read_block({0,0,{24,16}});
            const auto subset = subset_swath_geometry(std::get<SwathGeometry>(*loaded.product.frame.geometry),{24,0,{24,16}});
            auto projected = project_swath(subset,map,context);
            for (std::size_t i = 0; i < projected.vertices.values().size(); ++i) {
                require(block->vertices.values()[i].x == projected.vertices.values()[i].x);
                require(block->vertices.values()[i].y == projected.vertices.values()[i].y);
            }
        }
    }
}

void check_cold_ect(ExecutionContext& context,AdapterRegistry& registry) {
    const auto good = context.scratch().path()/"cold-good.nc",missing = context.scratch().path()/"cold-missing.nc";
    context.io().run([&] {
        for (const auto& path : {good,missing}) {
            metimage_fixture::create(path,true);
            H5::H5File file(path.string(),H5F_ACC_RDWR);
            for (int id : {16,17,18}) {
                const auto name = std::string(fy::sensors::metimage_channels[id]);
                const double wavelength = std::stod(name)/1000;
                const float raw = float(1000*metimage_fixture::radiance(wavelength,220)/metimage_fixture::radiance(wavelength,292));
                const std::vector<float> values(48*16,raw);
                file.openDataSet("data/measurement_data/vii_"+name).write(values.data(),H5::PredType::NATIVE_FLOAT);
            }
            if (path == missing) {
                const std::vector<float> values(48*16,0);
                file.openDataSet("data/measurement_data/vii_3740").write(values.data(),H5::PredType::NATIVE_FLOAT);
            }
        }
    });
    auto first = registry.open("metimage",{good,{},{}}),second = registry.open("metimage",{missing,{},{}});
    const auto definition = fy::products::mersi_product(first.metadata(),first.bands(),{.mode = "ECT"});
    auto reference = fy::products::load_product(first,definition,context);
    auto actual = fy::products::load_product(second,definition,context);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto& expected = reference.product.frame.planes[channel];
        const auto& observed = actual.product.frame.planes[channel];
        for (std::size_t i = 0; i < expected.valid.values().size(); ++i) {
            require(expected.valid.values()[i] == observed.valid.values()[i]);
            if (expected.valid.values()[i]) require(expected.values.values()[i] == observed.values.values()[i]);
        }
    }
}

}

int main(int argc,char** argv) try {
    if (argc >= 3 && std::string(argv[1]) == "--fixture") {
        metimage_fixture::create(argv[2],argc > 3 && std::string(argv[3]) == "night",argc > 4 ? std::stoi(argv[4]) : 0);
        return 0;
    }
    ExecutionContext context({.threads = 4}); AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry,context.profile());
    if (argc == 7 && std::string(argv[1]) == "--probe") {
        const auto path = fy::sensors::resolve_data_input(argv[2],context.scratch());
        auto session = registry.open("metimage",{path,{},{}});
        probe(session,{std::stoull(argv[3]),std::stoull(argv[4]),{std::stoull(argv[5]),std::stoull(argv[6])}},context);
        return 0;
    }
    const auto path = context.scratch().path()/"metimage.nc";
    context.io().run([&] { metimage_fixture::create(path); });
    check(path,context,registry);
    check_cold_ect(context,registry);
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
