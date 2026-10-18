/// SPDX-License-Identifier: MIT

#include "products/execute.hpp"
#include "sensors/hdf.hpp"
#include "product_reference.hpp"
#include "products/config.hpp"

#include <algorithm>
#include <cmath>
#include "hdf_fixture.hpp"

#include <iostream>
#include <source_location>

namespace {

namespace core = fy::core;

void require(bool valid,std::source_location location = std::source_location::current()) {
    if (!valid) throw std::runtime_error("Product check failed at line " + std::to_string(location.line()));
}

float radiance(float celsius,double wavenumber) {
    return static_cast<float>(1.1910429723971884140794892e-5 * wavenumber * wavenumber * wavenumber /
        (std::exp(1.4387768775039337 * wavenumber / (celsius + 273.15)) - 1));
}

void make_mersi(const std::filesystem::path& path,bool low_light,bool masked = false) {
    using namespace hdf_fixture;
    H5::H5File file(path.string(),H5F_ACC_TRUNC);
    metadata(file,low_light ? "FY-3E" : "FY-3H",low_light ? "MERSI-LL" : "MERSI");
    geolocation(file,"Geolocation",16,16);
    hdf_fixture::numbers(file,"Scan_Line_number",{16});
    hdf_fixture::numbers(file,"Pixels_per_Scan",{16});
    hdf_fixture::numbers(file,"EarthSun Distance Ratio",{1.012});
    hdf_fixture::numbers(file,"TBB_Trans_Coefficient",{1,1,1,1,1,1,0,0,0,0,0,0});
    const hsize_t spatial[]{16,16};
    if (low_light) {
        std::vector<float> values(256);
        for (std::size_t i = 0; i < values.size(); ++i) values[i] = 1 + i % 127;
        auto source = dataset(file,"Data/EV_1KM_LL",spatial,values);
        hdf_fixture::numbers(source,"Slope",{0.01}); hdf_fixture::numbers(source,"Intercept",{0});
        return;
    }
    for (auto name : {"SolarZenith","SolarAzimuth"}) {
        std::vector<float> angle(256,name == std::string("SolarZenith") ? 6000 : 4500);
        auto source = dataset(file,std::string("Geolocation/") + name,spatial,angle);
        hdf_fixture::numbers(source,"Slope",{0.01}); hdf_fixture::numbers(source,"Intercept",{0});
    }
    for (auto [name,channels,offset] : {std::tuple{"EV_250_Aggr.1KM_RefSB",4,0}, {"EV_1KM_RefSB",15,4}}) {
        const hsize_t dims[]{static_cast<hsize_t>(channels),16,16};
        std::vector<float> values(static_cast<std::size_t>(channels) * 256);
        for (int band = 0; band < channels; ++band)
            for (std::size_t i = 0; i < 256; ++i) values[band * 256 + i] = float(10 + (i * 3 + band + offset) % 80);
        auto source = dataset(file,std::string("Data/") + name,dims,values);
        hdf_fixture::numbers(source,"Slope",{1}); hdf_fixture::numbers(source,"Intercept",{0});
    }
    const hsize_t coefficient_dims[]{19,3};
    std::vector<float> coefficients(57);
    for (int band = 0; band < 19; ++band) coefficients[band * 3 + 1] = 1;
    dataset(file,"Calibration/VIS_Cal_Coeff",coefficient_dims,coefficients);
    for (auto [name,first,channels] : {std::tuple{"EV_1KM_Emissive",20,4}, {"EV_250_Aggr.1KM_Emissive",24,2}}) {
        const hsize_t dims[]{static_cast<hsize_t>(channels),16,16};
        std::vector<float> values(static_cast<std::size_t>(channels) * 256);
        for (int band = 0; band < channels; ++band) {
            const auto wavenumber = kSatConfigs.at("FY-3H").bands.at(first + band).mersi_equiv_mid_wn;
            for (std::size_t i = 0; i < 256; ++i) {
                const auto temperature = float(-30 + static_cast<int>((i * 7 + band * 5) % 55));
                values[band * 256 + i] = radiance(temperature,wavenumber);
            }
            if (masked) values[band * 256 + 255] = 65535;
        }
        auto source = dataset(file,std::string("Data/") + name,dims,values);
        hdf_fixture::numbers(source,"Slope",{1}); hdf_fixture::numbers(source,"Intercept",{0});
        if (masked) hdf_fixture::numbers(source,"FillValue",{65535});
    }
}

void make_mwri(const std::filesystem::path& path) {
    using namespace hdf_fixture;
    H5::H5File file(path.string(),H5F_ACC_TRUNC);
    metadata(file,"FY-3H","MWRI-II","010KM");
    for (auto [group,channels] : {std::pair{"Window Channel",10}, {"Sounding Channel",12}}) {
        geolocation(file,std::string(group) + "/Geolocation",16,16);
        const hsize_t dims[]{16,16,static_cast<hsize_t>(channels)};
        std::vector<float> values(256 * channels);
        for (std::size_t i = 0; i < 256; ++i)
            for (int channel = 0; channel < channels; ++channel)
                values[i * channels + channel] = float(700 + channel * 13 + i % 80);
        auto source = dataset(file,std::string(group) + "/Calibration/EARTH_OBSERVE_BT",dims,values,H5::PredType::NATIVE_INT16);
        hdf_fixture::numbers(source,"Slope",{0.1}); hdf_fixture::numbers(source,"Intercept",{170});
    }
}

void compare(const fy::products::PreparedProduct& native,const product_reference::Frame& expected,
             const std::string& mode,bool all_valid = true) {
    const auto& reference = expected.pixels;
    require(native.frame.planes.front().values.values().size() == reference.size());
    require(native.labels == expected.labels);
    const auto& geometry = std::get<core::SwathGeometry>(*native.frame.geometry);
    require(geometry.latitude.shape() == core::Shape{expected.rows,expected.cols});
    for (std::size_t i = 0; i < expected.lat.size(); ++i)
        require(geometry.latitude.values()[i] == expected.lat[i] && geometry.longitude.values()[i] == expected.lon[i]);
    if (native.display.colorbar)
        require(std::abs(native.display.scale.low - expected.scale.lowval) < 1e-5 &&
                std::abs(native.display.scale.high - expected.scale.hival) < 1e-5);
    std::vector<float> values(native.frame.planes.size());
    int maximum = 0;
    std::size_t valid = 0;
    for (std::size_t pixel = 0; pixel < reference.size(); ++pixel) {
        bool good = true;
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = native.frame.planes[i].values.values()[pixel];
            good = good && native.frame.planes[i].valid.values()[pixel];
        }
        if (!good) continue;
        if (native.frame.planes.size() == 1 && expected.values.size() == reference.size())
            require(std::abs(values.front() - expected.values[pixel]) <= 1e-5 + 2e-6 * std::abs(expected.values[pixel]));
        const auto color = native.display.mapper(values,native.display.scale);
        const std::array<double,3> channels{color.blue,color.green,color.red};
        for (std::size_t i = 0; i < 3; ++i) {
            require(std::isfinite(channels[i]));
            const auto value = static_cast<int>(std::clamp(channels[i] * 255,0.0,255.0));
            maximum = std::max(maximum,std::abs(value - reference[pixel][i]));
        }
        ++valid;
    }
    std::cout << mode << " valid=" << valid << " max quantized difference=" << maximum << '\n';
    require(valid > 0 && (!all_valid || valid == reference.size()));
    require(maximum <= 1);
}

}

int main() {
    core::ExecutionContext context({.threads = 4});
    const auto directory = context.scratch().path();
    auto mersi = directory / "FY3H_MERSI_20260618_0050_1000M_V0.HDF";
    auto low_light = directory / "FY3E_MERSI_20260618_0050_1000M_V0.HDF";
    auto mwri = directory / "mwri.HDF";
    auto masked = directory / "masked.HDF";
    context.io().run([&] { make_mersi(mersi,false); make_mersi(low_light,true); make_mersi(masked,false,true); make_mwri(mwri); });
    core::AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry,context.profile());
    for (const auto& mode : {"E","R","LL","TSG","RT","RY","RF","RMUL","EC","EK"}) {
        const auto path = mode == std::string("LL") ? low_light : mersi;
        auto session = registry.open("mersi",{path,path,{}});
        const auto band = mode == std::string("E") ? 24 : 3;
        fy::products::ProductRequest request;
        request.mode = mode; request.band = band;
        auto program = fy::products::mersi_product(session.metadata(),session.bands(),request);
        auto product = fy::products::load_product(session,program,context);
        auto reference = product_reference::next("swath");
        compare(product.product,reference,mode);
        require(product.product.labels.size() == 5);
    }
    for (const auto& mode : {"W","S","W84","TCX","TCN","TCN2","TCS","TCR"}) {
        auto session = registry.open("mwri",{mwri,mwri,{}});
        fy::products::ProductRequest request;
        request.mode = mode; request.band = 8;
        auto program = fy::products::mwri_product(session.metadata(),session.bands(),request);
        auto product = fy::products::load_product(session,program,context);
        auto reference = product_reference::next("swath");
        compare(product.product,reference,mode);
        auto cropped = fy::products::load_product(session,program,context,fy::products::GeographicRegion{17,147,2});
        auto crop_reference = product_reference::next("swath");
        compare(cropped.product,crop_reference,mode);
        require(cropped.source_window.row == 5 && cropped.source_window.col == 5 &&
                cropped.source_window.shape == core::Shape{5,5} && cropped.source_shape == core::Shape{16,16});
    }
    for (const auto& mode : {"E","RF","EK"}) {
        auto session = registry.open("mersi",{masked,masked,{}});
        fy::products::ProductRequest request;
        request.mode = mode; request.band = 24;
        auto program = fy::products::mersi_product(session.metadata(),session.bands(),request);
        auto product = fy::products::load_product(session,program,context);
        auto reference = product_reference::next("swath");
        compare(product.product,reference,mode,false);
        const auto& plane = product.product.frame.planes.back();
        require(!plane.valid.values().back() && std::isfinite(plane.values.values().back()));
    }
    const auto report = context.profile().snapshot();
    for (const auto& usage : *report.buffers) require(usage.current_bytes == 0);
}
