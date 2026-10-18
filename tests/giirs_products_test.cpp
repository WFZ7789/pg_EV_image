/// SPDX-License-Identifier: MIT

#include "products/giirs.hpp"
#include "sensors/hdf.hpp"
#include "product_reference.hpp"

#include <algorithm>
#include <cmath>
#include "giirs_fixture.hpp"

#include <iostream>
#include <source_location>

namespace {

namespace core = fy::core;

void require(bool condition,std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("GIIRS check failed at line " + std::to_string(location.line()));
}

template<class F> void rejects(F&& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected GIIRS rejection");
}

void check(const core::SourceFiles& files,const std::string& mode,int band,core::ExecutionContext& context) {
    core::AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry,context.profile());
    require(context.io().run([&] { return fy::sensors::identify_hdf_adapter(files); }) == "giirs");
    auto session = registry.open("giirs",files);
    auto definition = fy::products::giirs_product(session.metadata(),session.bands(),{.mode = mode,.band = band});
    auto loaded = fy::products::load_product(session,definition,context);
    const auto expected = product_reference::next("giirs");
    const auto& product = loaded.product;
    const auto& plane = product.frame.planes.front();
    const auto& geometry = std::get<core::SwathGeometry>(*product.frame.geometry);
    require(plane.values.shape() == core::Shape{std::size_t(expected.rows),std::size_t(expected.cols)});
    require(product.labels == expected.labels && product.display.scale.low == expected.scale.lowval &&
            product.display.scale.high == expected.scale.hival);
    require(plane.unit == (mode == "R" ? "1" : "K"));
    std::size_t common = 0;
    int maximum = 0;
    float difference = 0;
    for (std::size_t i = 0; i < expected.values.size(); ++i) {
        require(plane.values.values()[i] == expected.values[i] ||
                (std::isnan(plane.values.values()[i]) && std::isnan(expected.values[i])));
        require(geometry.latitude.values()[i] == expected.lat[i] && geometry.longitude.values()[i] == expected.lon[i]);
        if (!plane.valid.values()[i] || !geometry.valid.values()[i]) continue;
        ++common;
        difference = std::max(difference,std::abs(plane.values.values()[i] - expected.values[i]));
        const float value[]{plane.values.values()[i]};
        const auto color = product.display.mapper(value,product.display.scale);
        const double channels[]{color.blue,color.green,color.red};
        for (int channel = 0; channel < 3; ++channel)
            maximum = std::max(maximum,std::abs(int(std::lround(channels[channel] * 255)) - expected.pixels[i][channel]));
    }
    std::cout << mode << band << " common=" << common << " value/geo difference=" << difference << " color difference=" << maximum << std::endl;
    require(common > 0 && maximum == 0 && difference == 0);
}

struct SpectralReference {
    std::string family;
    std::vector<float> values;
    std::vector<float> wavenumbers;
    core::DisplayScale scale{INFINITY,-INFINITY};
};

SpectralReference sequence_reference(const std::filesystem::path& path,const std::string& family) {
    static_cast<void>(path);
    auto frozen = product_reference::next("spectrum");
    SpectralReference result;
    result.family = family;
    result.values = std::move(frozen.values);
    result.wavenumbers = std::move(frozen.wavenumbers);
    result.scale = {frozen.scale.lowval,frozen.scale.hival};
    return result;
}

void sequence(const std::filesystem::path& path,core::ExecutionContext& context,bool fail = false) {
    core::AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry,context.profile());
    auto session = registry.open("giirs",{path,path,{}});
    const auto expected = context.io().run([&] { return std::array{sequence_reference(path,"MW"),sequence_reference(path,"LW")}; });
    const auto before = context.profile().snapshot();
    std::size_t frames = 0;
    auto consume = [&](const fy::products::PreparedProduct& product,const core::FramePlan& frame) {
        const auto& reference = expected[frame.grid == "MW" ? 0 : 1];
        const auto& plane = product.frame.planes.front();
        require(product.display.scale.low == reference.scale.low && product.display.scale.high == reference.scale.high);
        require(frame.name == "00000" + std::to_string(frames) && product.frame.name == frame.name);
        require(product.labels.back().find(std::to_string(reference.wavenumbers[*frame.spectral_channel]) + "cm^-1") != std::string::npos);
        for (std::size_t i = 0; i < plane.values.shape().elements(); ++i) {
            const float expected_value = reference.values[*frame.spectral_channel * plane.values.shape().elements() + i];
            require(plane.values.values()[i] == expected_value || (std::isnan(plane.values.values()[i]) && std::isnan(expected_value)));
        }
        ++frames;
        if (fail) throw std::runtime_error("consumer failure");
    };
    if (fail) rejects([&] { fy::products::stream_giirs_sequence(session,context,consume); });
    else fy::products::stream_giirs_sequence(session,context,consume);
    const auto after = context.profile().snapshot();
    require(after.cache->current_bytes == 0 && !std::filesystem::exists(context.scratch().path() / "giirs-MW.frames") &&
            !std::filesystem::exists(context.scratch().path() / "giirs-LW.frames"));
    if (!fail) {
        require(frames == 5 && *after.source_pixels == 5 * 24);
        require(after.io->read_samples - before.io->read_samples == 2 * 2 * 24 + 5 * 24);
        require(after.cache->write_bytes - (before.cache ? before.cache->write_bytes : 0) == 5 * 24 * 5);
        std::cout << "IRall frames=" << frames << " MW/LW family scales and values exact; each channel read once\n";
    }
}

}

int main() {
    core::ExecutionContext context({.threads = 4});
    context.io().run([] { static_cast<void>(H5::PredType::NATIVE_FLOAT.getSize()); });
    const auto initial = context.io().run([] { return H5Fget_obj_count(H5F_OBJ_ALL,H5F_OBJ_ALL); });
    const auto path = context.scratch().path() / "giirs.HDF";
    for (bool masked : {false,true}) {
        context.io().run([&] { giirs_fixture::write(path,masked); });
        check({path,path,{}},"R",0,context);
        check({path,path,{}},"LW",1,context); check({path,path,{}},"MW",1,context);
        sequence(path,context); sequence(path,context,true);
    }
    context.io().run([&] { giirs_fixture::write(path,false,true); });
    sequence(path,context);
    context.io().run([&] { giirs_fixture::write(path,false,false,false,true); });
    check({path,path,{}},"R",0,context);
    {
        core::ExecutionContext bounded({.threads = 4});
        sequence(path,bounded);
        const auto snapshot = bounded.profile().snapshot();
        require((*snapshot.buffers)[static_cast<std::size_t>(core::BufferKind::scene)].peak_bytes <= 19 * 24);
        require(snapshot.cache->peak_bytes <= 3 * 24 * 5);
    }
    {
        core::AdapterRegistry registry;
        fy::sensors::register_hdf_adapters(registry,context.profile());
        auto session = registry.open("giirs",{path,path,{}});
        const auto metadata = session.metadata();
        const auto bands = session.bands();
        require(metadata.grid("LW").shape == core::Shape{4,6} && metadata.grid("VIS").shape == core::Shape{8,10});
        const auto spectral = std::ranges::find(bands,fy::sensors::giirs_lw_offset + 1,&core::BandDescriptor::id);
        require(spectral != bands.end() && spectral->channel_axis == core::ChannelAxis::first && spectral->channel_index == 1);
        auto block = session.read_window(spectral->id,{1,2,{2,3}});
        require(std::abs(block.values.values().front() - 40.6f) < 1e-5);
        auto navigation = session.read_geometry("LW",{1,2,{2,3}});
        const auto& geometry = std::get<core::SwathGeometry>(navigation);
        require(geometry.latitude.values().front() == 10.3f && geometry.longitude.values().front() == 140.8f);
        const auto table = std::ranges::find(bands,4,&core::BandDescriptor::id);
        require(table != bands.end() && table->channel_axis == core::ChannelAxis::last);
        auto coefficient = session.read_window(4,{2,3,{2,3}});
        require(coefficient.values.values().front() == 2 + .01f * 3);
        rejects([&] { fy::products::giirs_product(metadata,bands,{.mode = "LW",.band = 3}); });
        rejects([&] { fy::products::giirs_product(metadata,bands,{.mode = "MW",.band = -1}); });
        rejects([&] { fy::products::giirs_product(metadata,bands,{.mode = "E"}); });
    }
    context.io().run([&] { giirs_fixture::write(path,false,false,true); });
    {
        core::AdapterRegistry registry;
        fy::sensors::register_hdf_adapters(registry,context.profile());
        auto session = registry.open("giirs",{path,path,{}});
        auto definition = fy::products::giirs_product(session.metadata(),session.bands(),{.mode = "LW",.band = 1});
        rejects([&] { fy::products::load_product(session,definition,context); });
        rejects([&] { fy::products::stream_giirs_sequence(session,context,[](const auto&,const auto&) {}); });
        require(context.profile().snapshot().cache->current_bytes == 0);
    }
    context.io().run([&] {
        giirs_fixture::write(path);
        H5::H5File file(path.string(),H5F_ACC_RDWR);
        const std::array<float,3> invalid{-100,750,800};
        file.openDataSet("Data/WN_LW").write(invalid.data(),H5::PredType::NATIVE_FLOAT);
    });
    {
        core::AdapterRegistry registry;
        fy::sensors::register_hdf_adapters(registry,context.profile());
        rejects([&] { registry.open("giirs",{path,path,{}}); });
    }
    require(context.io().run([] { return H5Fget_obj_count(H5F_OBJ_ALL,H5F_OBJ_ALL); }) == initial);
}
