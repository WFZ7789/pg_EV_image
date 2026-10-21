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
    for (auto name : {"SolarZenith","SolarAzimuth","SensorZenith","SensorAzimuth"}) {
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
        if (mode != std::string("RT")) compare(product.product,reference,mode);
        require(product.product.labels.size() == 5);
    }
    {
        auto session = registry.open("mersi",{mersi,mersi,{}});
        auto metadata = session.metadata();
        metadata.numeric_attributes["earth_sun_distance_au"] = 1;
        const core::Shape shape{16,16};
        core::SwathGeometry positions{core::Raster<float>(shape),core::Raster<float>(shape),core::Raster<std::uint8_t>(shape)};
        auto geometry = std::make_shared<core::NativeGeometry>(std::move(positions));
        auto inputs = [&] {
            std::vector<core::ScalarPlane> planes;
            for (int channel = 0; channel < 3; ++channel) {
                planes.push_back({"%",core::Raster<float>(shape),core::Raster<std::uint8_t>(shape)});
                std::fill(planes.back().values.values().begin(),planes.back().values.values().end(),float(50 + channel * 50));
                std::fill(planes.back().valid.values().begin(),planes.back().valid.values().end(),1);
            }
            planes[0].values.values()[0] = -50;
            planes[2].values.values()[1] = 250;
            planes[1].values.values()[2] = 250.01f;
            planes[0].values.values()[3] = -10000; planes[0].valid.values()[3] = 0;
            planes[2].values.values()[4] = NAN;
            for (int angle = 0; angle < 4; ++angle) {
                planes.push_back({"degree",core::Raster<float>(shape),core::Raster<std::uint8_t>(shape)});
                std::fill(planes.back().valid.values().begin(),planes.back().valid.values().end(),1);
            }
            return planes;
        };
        for (bool mosaic : {false,true}) {
            auto program = fy::products::mersi_product(metadata,session.bands(),{.mode = "RT",.color = "linear",.mosaic = mosaic});
            auto product = program.prepare(inputs(),geometry,context);
            require(product.display.scale.low == 0 && product.display.scale.high == 2.5);
            require(product.display.balanced_rgb);
            require(!product.shared_percentile_plane);
            require(!product.frame.planes[1].valid.values()[2] && std::isnan(product.frame.planes[1].values.values()[2]));
            require(!product.frame.planes[0].valid.values()[3] && !product.frame.planes[2].valid.values()[4]);
            const std::array<float,3> sample{0.5f,1,1.5f};
            auto color = product.display.mapper(sample,product.display.scale);
            require(std::abs(color.red - 0.6) < 1e-6 && std::abs(color.green - 0.4) < 1e-6 &&
                    std::abs(color.blue - 0.2) < 1e-6);
            const std::array<float,3> extremes{-10,10,2.5f};
            color = product.display.mapper(extremes,product.display.scale);
            require(color.red == 1 && color.green == 1 && color.blue == 0);
            auto invalid = inputs();
            for (auto& plane : invalid)
                std::fill(plane.valid.values().begin(),plane.valid.values().end(),0);
            bool rejected = false;
            try { program.prepare(std::move(invalid),geometry,context); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected);
        }
        for (const std::string curve : {"srgb","contrast"}) {
            auto program = fy::products::mersi_product(metadata,session.bands(),{.mode = "RT",.color = curve});
            auto product = program.prepare(inputs(),geometry,context);
            const std::array<float,3> middle{0.5f,0.5f,0.5f};
            const auto color = product.display.mapper(middle,{0,1});
            require(std::abs(color.red - 0.73535698) < 1e-6 && color.red == color.green && color.green == color.blue);
            double previous = -1;
            for (int i = 0; i <= 512; ++i) {
                const float value = float(i) / 512;
                const std::array<float,3> sample{value,value,value};
                const auto mapped = product.display.mapper(sample,{0,1});
                require(std::isfinite(mapped.red) && mapped.red >= previous && mapped.red >= 0 && mapped.red <= 1);
                previous = mapped.red;
            }
        }
    }
    {
        auto session = registry.open("mersi",{mersi,mersi,{}});
        const core::Shape shape{16,16};
        core::SwathGeometry positions{core::Raster<float>(shape),core::Raster<float>(shape),core::Raster<std::uint8_t>(shape)};
        auto geometry = std::make_shared<core::NativeGeometry>(std::move(positions));
        for (const auto& satellite : {"FY-3D","FY-3H"}) for (bool auxiliary : {false,true}) {
            auto metadata = session.metadata();
            metadata.satellite = satellite;
            auto bands = session.bands();
            if (auxiliary) {
                metadata.grids.push_back({"auxiliary",shape,core::SourceRole::auxiliary,"geo"});
                for (int id : {24,25}) {
                    auto band = *std::ranges::find(bands,id,&core::BandDescriptor::id);
                    band.id += fy::sensors::auxiliary_band_offset; band.grid = "auxiliary";
                    band.source = core::SourceRole::auxiliary;
                    bands.push_back(std::move(band));
                }
            }
            auto program = fy::products::mersi_product(metadata,bands,{.mode = "EC"});
            auto inputs = [&](float temperature,float middle,bool middle_valid) {
                std::vector<core::ScalarPlane> planes;
                for (const auto& dependency : program.plan.inputs) {
                    const bool angle = dependency.band == fy::sensors::solar_zenith || dependency.band == fy::sensors::solar_azimuth;
                    const bool mid = dependency.band == 20;
                    planes.push_back({angle ? "degree" : "K",core::Raster<float>(shape),core::Raster<std::uint8_t>(shape)});
                    std::fill(planes.back().values.values().begin(),planes.back().values.values().end(),
                              angle ? 0.0f : (mid ? middle : temperature) + 273.15f);
                    std::fill(planes.back().valid.values().begin(),planes.back().valid.values().end(),mid ? middle_valid : 1);
                }
                return planes;
            };
            for (float temperature : {-90.0f,-75.0f,-60.0f}) {
                auto expected = program.prepare(inputs(temperature,20,true),geometry,context);
                for (auto [middle,valid] : {std::pair{10000.0f,true},{10000.0f,false},{float(NAN),false},
                                          {float(NAN),true},{float(INFINITY),true}}) {
                    auto actual = program.prepare(inputs(temperature,middle,valid),geometry,context);
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        for (std::size_t i = 0; i < shape.elements(); ++i) {
                            require(actual.frame.planes[channel].valid.values()[i]);
                            require(std::isfinite(actual.frame.planes[channel].values.values()[i]));
                            require(actual.frame.planes[channel].values.values()[i] == expected.frame.planes[channel].values.values()[i]);
                        }
                }
                auto missing = inputs(temperature,NAN,false);
                const auto infrared = std::ranges::find(program.plan.inputs,24 + (auxiliary ? fy::sensors::auxiliary_band_offset : 0),
                                                       &core::InputDependency::band) - program.plan.inputs.begin();
                missing[infrared].valid.values()[0] = 0;
                auto actual = program.prepare(std::move(missing),geometry,context);
                require(!actual.frame.planes[0].valid.values()[0] && actual.frame.planes[0].valid.values()[1]);
            }
            auto warm = program.prepare(inputs(-20,10,true),geometry,context);
            auto changed = program.prepare(inputs(-20,30,true),geometry,context);
            require(std::abs(warm.frame.planes[0].values.values()[0] - changed.frame.planes[0].values.values()[0]) > 0.01f);
            auto missing = program.prepare(inputs(-20,NAN,false),geometry,context);
            require(!missing.frame.planes[0].valid.values()[0]);
            auto below = program.prepare(inputs(-60.001f,20,true),geometry,context);
            auto above = program.prepare(inputs(-59.999f,20,true),geometry,context);
            for (std::size_t channel = 0; channel < 3; ++channel)
                require(std::abs(below.frame.planes[channel].values.values()[0] - above.frame.planes[channel].values.values()[0]) < 0.001f);
        }
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
