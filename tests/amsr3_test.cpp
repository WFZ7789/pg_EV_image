/// SPDX-License-Identifier: MIT

#include "amsr3_fixture.hpp"
#include "native_swath.hpp"
#include "sensors/hdf.hpp"
#include "core/calibration.hpp"
#include "products/execute.hpp"
#include "render_product.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <iostream>
#include <source_location>

namespace {
using namespace fy::core;
void require(bool condition,std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("AMSR3 check failed at "+std::to_string(location.line()));
}
template<class F> void rejects(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    require(rejected);
}

void samples(const std::filesystem::path& directory) {
    ExecutionContext context({.threads = 4});
    AdapterRegistry registry; fy::sensors::register_hdf_adapters(registry);
    std::size_t files = 0,bands = 0;
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        if (!item.path().filename().string().starts_with("GGWAM3") || item.path().extension() != ".nc") continue;
        const auto path = item.path();
        require(context.io().run([&] { return fy::sensors::identify_hdf_adapter({path,{},{}}); }) == "amsr3");
        auto session = registry.open("amsr3",{path,{},{}});
        const auto metadata = session.metadata();
        require(session.bands().size() == 21);
        for (const auto& band : session.bands()) {
            const auto shape = metadata.grid(band.grid).shape;
            const Window window{shape.rows/2,shape.cols/2,{3,3}};
            auto block = session.read_window(band.id,window);
            const auto reference = context.io().run([&] {
                H5::H5File file(path.string(),H5F_ACC_RDONLY);
                auto dataset = file.openDataSet(band.dataset),quality = file.openDataSet(band.dataset+"_Quality");
                const hsize_t origin[]{window.row,window.col},count[]{3,3};
                H5::DataSpace memory(2,count);
                auto selection = dataset.getSpace(); selection.selectHyperslab(H5S_SELECT_SET,count,origin);
                std::array<float,9> raw; dataset.read(raw.data(),H5::PredType::NATIVE_FLOAT,memory,selection);
                return raw;
            });
            for (std::size_t i = 0; i < 9; ++i) require(reference[i] == block.values.values()[i]);
            calibrate(block,band.calibration,context);
            for (std::size_t i = 0; i < 9; ++i) if (block.valid.values()[i])
                require(std::abs(block.values.values()[i]-reference[i]*.01) < .001);
            auto geometry = session.read_geometry(band.grid,window);
            require(geometry_shape(geometry) == window.shape);
            ++bands;
        }
        ++files; std::cout << path.filename() << ": 21 channel windows/calibration/geolocation passed\n";
    }
    require(files == 8 && bands == 168);
}
}

int main(int argc,char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--fixture") { amsr3_fixture::create(argv[2]); return 0; }
    if (argc == 3 && std::string(argv[1]) == "--samples") { samples(argv[2]); return 0; }
    using namespace fy::core;
    ScratchDirectory temporary;
    const auto path = temporary.path()/"amsr3.nc";
    amsr3_fixture::create(path);
    AdapterRegistry registry; fy::sensors::register_hdf_adapters(registry);
    ExecutionContext context({.threads = 4});
    auto session = registry.open("amsr3",{path,{},{}});
    const auto metadata = session.metadata(); const auto bands = session.bands();
    require(metadata.satellite == "GOSAT-GW" && metadata.sensor == "AMSR3" && bands.size() == 21);
    auto raw = session.read_window(0,{0,0,{2,8}});
    require(!raw.valid.values()[0] && !raw.valid.values()[1] && raw.valid.values()[2]);
    for (std::size_t i = 3; i < 8; ++i) require(!raw.valid.values()[i]);
    require(raw.valid.values()[8] && raw.values.values()[8] == 22020);
    calibrate(raw,bands[0].calibration,context);
    require(std::isnan(raw.values.values()[0]) && std::abs(raw.values.values()[8]-220.2) < .001);
    const auto bad_scan = session.read_window(14,{5,0,{1,16}});
    require(!bad_scan.valid.values()[0]);
    auto geo = std::get<SwathGeometry>(session.read_geometry("165",{0,0,{2,8}}));
    require(!geo.valid.values()[0] && geo.valid.values()[1]);
    rejects([&] { session.read_window(21,{0,0,{1,1}}); });
    rejects([&] { session.read_window(0,{6,0,{1,1}}); });
    rejects([&] { registry.open("amsr3",{path,path.parent_path()/"geo.nc",{}}); });
    for (const auto& band : bands) {
        const auto definition = fy::products::amsr3_product(metadata,bands,{.mode = "W",.band = band.id});
        auto loaded = fy::products::load_product(session,definition,context);
        require(loaded.product.frame.planes.size() == 1 && loaded.product.display.colorbar);
        require(loaded.product.display.scale.low < loaded.product.display.scale.high);
    }
    {
        auto definition = fy::products::amsr3_product(metadata,bands,{.mode = "W",.band = 8});
        definition.plan.inputs.front().band = 10;
        auto aligned = fy::products::load_product(session,definition,context);
        const auto value = aligned.product.frame.planes.front().values.view().at(2,4);
        require(std::isfinite(value) && std::abs(value-230.6f) > .0002f);
    }
    {
        const auto definition = fy::products::amsr3_product(metadata,bands,{.mode = "W",.band = 14});
        const fy::products::GeographicRegion region{10.5,140.6,.35};
        auto loaded = fy::products::load_product(session,definition,context,region);
        const auto& geometry = std::get<SwathGeometry>(*loaded.product.frame.geometry);
        const auto& plane = loaded.product.frame.planes.front();
        require(std::ranges::count(geometry.valid.values(),0) > 0);
        for (std::size_t i = 0; i < geometry.valid.values().size(); ++i)
            if (plane.valid.values()[i]) {
                require(std::abs(geometry.latitude.values()[i]-region.latitude) <= region.half_range);
                require(std::abs(geometry.longitude.values()[i]-region.longitude) <= region.half_range);
            }
    }
    for (const auto& mode : {"W84","TCX","TCS","TCR"})
        for (const auto& scan : {"A","B"}) {
            const auto definition = fy::products::amsr3_product(metadata,bands,{.mode = mode,.scan = scan});
            auto loaded = fy::products::load_product(session,definition,context);
            const auto shape = loaded.product.frame.planes[0].values.shape(); require(shape == Shape{6,16});
            const std::size_t i = 2*16+4;
            std::vector<float> values;
            for (const auto& plane : loaded.product.frame.planes) { require(plane.valid.values()[i]); values.push_back(plane.values.values()[i]); }
            const auto color = loaded.product.display.mapper(values,loaded.product.display.scale);
            auto mwri_metadata = metadata;
            mwri_metadata.satellite = "FY-3D"; mwri_metadata.sensor = "MWRI";
            std::vector<BandDescriptor> mwri_bands(10,bands.front());
            for (int id = 0; id < 10; ++id) {
                mwri_bands[id].id = id; mwri_bands[id].grid = "89A";
            }
            auto reference_definition = fy::products::mwri_product(mwri_metadata,mwri_bands,{.mode = mode});
            std::vector<ScalarPlane> reference_inputs;
            for (float value : values) {
                ScalarPlane plane{"K",Raster<float>(shape),Raster<std::uint8_t>(shape)};
                std::ranges::fill(plane.values.values(),value); std::ranges::fill(plane.valid.values(),1);
                reference_inputs.push_back(std::move(plane));
            }
            auto reference = reference_definition.prepare(std::move(reference_inputs),loaded.product.frame.geometry,context);
            const auto expected_color = reference.display.mapper(values,reference.display.scale);
            require(std::abs(color.red-expected_color.red) < 1e-6 &&
                    std::abs(color.green-expected_color.green) < 1e-6 && std::abs(color.blue-expected_color.blue) < 1e-6);
            const auto normalize = [](double v) { return std::clamp(v,0.,255.)/255.; };
            if (std::string(mode) == "W84") {
                require(std::abs(color.red-normalize((300-(1.818*values[0]-.818*values[1]))*1.5)) < 1e-5);
                require(std::abs(color.green-normalize((300-values[0])*1.5)) < 1e-5);
            }
        }
    {
        AdapterRegistry measured_registry;
        ExecutionContext measured({.threads = 1});
        fy::sensors::register_hdf_adapters(measured_registry,measured.profile());
        auto measured_session = measured_registry.open("amsr3",{path,{},{}});
        fy::products::ProductLoadCache cache;
        const auto definition_a = fy::products::amsr3_product(metadata,bands,{.mode = "TCS",.scan = "A"});
        const auto definition_b = fy::products::amsr3_product(metadata,bands,{.mode = "TCS",.scan = "B"});
        auto a = fy::products::load_product(measured_session,definition_a,measured,{},&cache);
        require(cache.planes.size() == 3 && cache.geometry.size() == 3);
        const auto before = measured.profile().snapshot().io->read_samples;
        auto b = fy::products::load_product(measured_session,definition_b,measured,{},&cache);
        const auto after = measured.profile().snapshot().io->read_samples;
        require(after-before == 4*6*16);
    }
    for (const auto& mode : {"S","TCN","TCN2","E"})
        rejects([&] { fy::products::amsr3_product(metadata,bands,{.mode = mode}); });
    rejects([&] { fy::products::amsr3_product(metadata,bands,{.mode = "W",.band = -1}); });
    for (const auto& mode : {"W84","TCX","TCS","TCR"}) {
        cv::Mat reference;
        for (int threads : {1,4,0}) {
            ExecutionContext render_context({.threads = threads});
            fy::Request request;
            request.inputs.push_back({path,{},{}}); request.mode = mode; request.amsr3_scan = "AB";
            request.width = 256; request.output = temporary.path()/(std::string(mode)+std::to_string(threads)+".png");
            fy::render_native_scene(request,"amsr3",render_context);
            const auto image = cv::imread(request.output.string()); require(!image.empty() && image.cols == 276);
            if (reference.empty()) reference = image;
            else require(cv::norm(reference,image,cv::NORM_INF) == 0);
        }
    }
    {
        auto definition = fy::products::amsr3_product(metadata,bands,{.mode = "W84"});
        std::vector<fy::products::LoadedProduct> products;
        products.push_back(fy::products::load_product(session,definition,context));
        products.push_back(fy::products::load_product(session,definition,context));
        products[1].product.frame.geometry = products[0].product.frame.geometry;
        for (auto& product : products)
            for (auto& plane : product.product.frame.planes)
                for (auto& value : plane.values.values()) if (std::isfinite(value)) value = 250;
        fy::Request request;
        request.mode = "W84"; request.width = 256; request.output = temporary.path()/"priority-A.png";
        fy::render_swath_products(request,products,context);
        const auto a = cv::imread(request.output.string());
        for (auto& plane : products[1].product.frame.planes)
            for (auto& value : plane.values.values()) if (std::isfinite(value)) value = 100;
        request.output = temporary.path()/"priority-AB.png";
        fy::render_swath_products(request,products,context);
        const auto ab = cv::imread(request.output.string());
        require(cv::norm(a,ab,cv::NORM_INF) == 0);
        for (auto& plane : products[0].product.frame.planes) {
            for (std::size_t row = 0; row < 6; ++row)
                for (std::size_t col = 8; col < 16; ++col) plane.valid.view().at(row,col) = 0;
        }
        request.output = temporary.path()/"union-AB.png";
        fy::render_swath_products(request,products,context);
        const auto combined = cv::imread(request.output.string());
        require(cv::norm(a,combined,cv::NORM_INF) > 0);
    }
    const auto wrong = temporary.path()/"wrong.nc"; amsr3_fixture::create(wrong);
    {
        H5::H5File file(wrong.string(),H5F_ACC_RDWR);
        auto root = file.openGroup("/"); root.removeAttr("processing_level");
        hdf_fixture::text(root,"processing_level","Level1R");
    }
    rejects([&] { registry.open("amsr3",{wrong,{},{}}); });
    std::cout << "AMSR3 adapters, products and A/B/AB rendering passed\n";
}
