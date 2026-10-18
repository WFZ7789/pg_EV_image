/// SPDX-License-Identifier: MIT

#include "products/execute.hpp"
#include "products/pmr.hpp"
#include "sensors/hdf.hpp"
#include "product_reference.hpp"

#include <algorithm>
#include <cmath>
#include "pmr_fixture.hpp"

#include <iostream>
#include <source_location>

namespace {

using namespace fy::core;

void require(bool condition,std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("PMR check failed at line " + std::to_string(location.line()));
}

template<class F> void rejects(F&& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected PMR rejection");
}

void check(const SourceFiles& files,float latitude,float longitude,float half,ExecutionContext& context) {
    AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry,context.profile());
    require(context.io().run([&] { return fy::sensors::identify_hdf_adapter(files); }) == "pmr");
    auto session = registry.open("pmr",files);
    const auto metadata = session.metadata();
    const auto definition = fy::products::pmr_product(metadata,session.bands(),{.mode = "PMR"});
    auto loaded = fy::products::load_product(session,definition,context,
                          fy::products::GeographicRegion{latitude,longitude,half});
    const auto expected = product_reference::next("pmr");
    const auto& product = loaded.product;
    const auto& geometry = std::get<SwathGeometry>(*product.frame.geometry);
    const auto& plane = product.frame.planes.front();
    require(plane.unit == "dBZ");
    require(plane.values.shape() == Shape{std::size_t(expected.rows),std::size_t(expected.cols)});
    require(product.labels == expected.labels && product.display.colorbar);
    require(product.display.scale.low == expected.scale.lowval && product.display.scale.high == expected.scale.hival);
    int maximum = 0;
    std::size_t common = 0;
    for (std::size_t i = 0; i < expected.values.size(); ++i) {
        require(plane.values.values()[i] == expected.values[i]);
        require(geometry.latitude.values()[i] == expected.lat[i]);
        require(std::remainder(geometry.longitude.values()[i] - expected.lon[i],360.0) == 0);
        if (!plane.valid.values()[i] || !geometry.valid.values()[i]) continue;
        ++common;
        const float value[]{plane.values.values()[i]};
        const auto color = product.display.mapper(value,product.display.scale);
        const double channels[]{color.blue,color.green,color.red};
        for (int channel = 0; channel < 3; ++channel)
            maximum = std::max(maximum,std::abs(int(std::lround(channels[channel] * 255)) - expected.pixels[i][channel]));
    }
    require(maximum == 0 && common > 0);
    if (metadata.labels.front() == "synthetic.HDF" && half == 1.1f) {
        require(loaded.source_window.row == 3 && loaded.source_window.col == 6 && plane.values.shape() == Shape{5,5});
        require(product.display.scale.high < 490);
    }
    std::cout << "PMR samples=" << expected.values.size() << " common=" << common
              << " values/geo max difference=0 color max difference=" << maximum << '\n';
}

}

int main() {
    ExecutionContext context({.threads = 4});
    context.io().run([] { static_cast<void>(H5::PredType::NATIVE_FLOAT.getSize()); });
    const auto initial = context.io().run([] { return H5Fget_obj_count(H5F_OBJ_ALL,H5F_OBJ_ALL); });
    const auto path = context.scratch().path() / "FY3G_PMR_20260730_0123_5000M_V0.HDF";
    context.io().run([&] { pmr_fixture::write(path); });
    check({path,path,{}},15,140,1.1f,context);
    check({path,path,{}},15,140,7.5f,context);
    AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry,context.profile());
    for (bool two_dimensional : {false,true}) {
        context.io().run([&] { pmr_fixture::write(path,true,true,true,true,two_dimensional); });
        auto session = registry.open("pmr",{path,{},{}});
        require(session.metadata().sensor == "PMR" && session.metadata().resolution == "5000M");
        const auto reads = context.profile().snapshot().io->read_samples;
        auto coordinates = session.read_geometry("native",{0,0,{12,16}});
        require(context.profile().snapshot().io->read_samples - reads == 2 * 12 * 16);
        const auto& geometry = std::get<SwathGeometry>(coordinates);
        require(geometry.latitude.values()[0] == 15 && !geometry.valid.values()[0]);
        require(geometry.longitude.values()[1] == 136.5 && geometry.valid.values()[1]);
        auto block = session.read_window(1,{4,7,{3,3}});
        require(block.values.values()[4] == 200 && !block.valid.values()[4]);
        auto definition = fy::products::pmr_product(session.metadata(),session.bands(),{.mode = "PMR"});
        auto product = fy::products::load_product(session,definition,context,fy::products::GeographicRegion{15,140,1.1});
        require(product.product.display.scale.high == 90);
        require(!product.product.frame.planes.front().valid.values()[12]);
        require(product.product.frame.planes.front().values.values()[12] == 90);
        rejects([&] { fy::products::pmr_product(session.metadata(),session.bands(),{.mode = "E"}); });
        rejects([&] { fy::products::load_product(session,definition,context,fy::products::GeographicRegion{-70,0,1}); });
    }
    context.io().run([&] {
        pmr_fixture::write(path,false,true);
        H5::H5File file(path.string(),H5F_ACC_RDWR);
        H5Adelete(file.getId(),"Sensor Identification Code");
        hdf_fixture::text(file,"Sensor Identification Code","PMR-Ku");
        const std::vector<float> zero(12 * 16,20);
        file.openDataSet("SLV/zFactorCorrectedESurface").write(zero.data(),H5::PredType::NATIVE_FLOAT);
    });
    check({path,path,{}},15,140,1.1f,context);
    context.io().run([&] {
        H5::H5File file(path.string(),H5F_ACC_RDWR);
        std::vector<float> invalid(12 * 16,NAN);
        file.openDataSet("SLV/zFactorCorrectedESurface").write(invalid.data(),H5::PredType::NATIVE_FLOAT);
    });
    {
        auto session = registry.open("pmr",{path,{},{}});
        auto definition = fy::products::pmr_product(session.metadata(),session.bands(),{.mode = "PMR"});
        rejects([&] { fy::products::load_product(session,definition,context,fy::products::GeographicRegion{15,140,1.1}); });
    }
    const auto geo = context.scratch().path() / "geo.HDF";
    context.io().run([&] { pmr_fixture::write(path,false,false,false); pmr_fixture::write(geo,false,false,true,false); });
    check({path,geo,{}},15,140,1.1f,context);
    rejects([&] { context.io().run([&] { fy::sensors::identify_hdf_adapter({path,{},{}}); }); });
    rejects([&] { registry.open("pmr",{path,{},{}}); });
    require(context.io().run([] { return H5Fget_obj_count(H5F_OBJ_ALL,H5F_OBJ_ALL); }) == initial);
}
