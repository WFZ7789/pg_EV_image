/// SPDX-License-Identifier: MIT

#include "native_swath.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cmath>
#include <iostream>
#include <source_location>
#include <thread>

namespace {

using namespace fy::core;

void require(bool value,std::source_location location = std::source_location::current()) {
    if (!value) throw std::runtime_error("New sensor check failed at line " + std::to_string(location.line()));
}

struct Counters {
    std::thread::id worker;
    std::size_t opened = 0,closed = 0,windows = 0,samples = 0,geometries = 0;
    bool serial = true;
    void touch() { serial = serial && std::this_thread::get_id() == worker; }
};

class NovelAdapter final : public SensorAdapter {
public:
    NovelAdapter(bool image,std::shared_ptr<Counters> counters,ProfileReport report)
        : image_(image),counters_(std::move(counters)),report_(std::move(report)) {
        counters_->worker = std::this_thread::get_id(); ++counters_->opened;
    }
    ~NovelAdapter() override { counters_->touch(); ++counters_->closed; }
    SensorMetadata metadata() const override {
        counters_->touch();
        SensorMetadata result;
        result.satellite = "TEST-SAT"; result.sensor = "NOVEL-INSTRUMENT"; result.resolution = "test-grid";
        result.observation_begin = "2026-10-18T00:00:00";
        result.labels = {"memory-scene","2026-10-18","00:00:00(UTC)","TEST-SAT NOVEL-INSTRUMENT","Arbitrary physical quantity"};
        result.grids.push_back({.id = "observations",.shape = shape()});
        return result;
    }
    std::vector<BandDescriptor> bands() const override {
        counters_->touch();
        BandDescriptor band;
        band.id = 73; band.name = "Arbitrary physical quantity"; band.dataset = "memory-values";
        band.grid = "observations"; band.unit = "DN"; band.calibrated_unit = "test-unit"; band.fill_values = {-9999};
        band.calibration.kind = CalibrationKind::linear; band.calibration.coefficients = {.5,7};
        return {band};
    }
    SampleBlock read_window(int band,Window window) override {
        counters_->touch(); window.validate(shape()); require(band == 73);
        ++counters_->windows; counters_->samples += window.shape.elements();
        SampleBlock block{band,window,Raster<float>(window.shape),Raster<std::uint8_t>(window.shape)};
        for (std::size_t row = 0; row < window.shape.rows; ++row)
            for (std::size_t col = 0; col < window.shape.cols; ++col) {
                const auto index = (row + window.row) * shape().cols + col + window.col;
                block.values.view().at(row,col) = index ? float(10 + index % 47) : -9999;
                block.valid.view().at(row,col) = index != 0;
            }
        const auto bytes = block.values.bytes();
        report_.record_io({bytes,bytes,1,window.shape.elements()});
        return block;
    }
    NativeGeometry read_geometry(const std::string& grid,Window window) override {
        counters_->touch(); window.validate(shape()); require(grid == "observations"); ++counters_->geometries;
        if (image_) return ImageGeometry{window.shape};
        SwathGeometry result{Raster<float>(window.shape),Raster<float>(window.shape),Raster<std::uint8_t>(window.shape)};
        for (std::size_t row = 0; row < window.shape.rows; ++row)
            for (std::size_t col = 0; col < window.shape.cols; ++col) {
                result.latitude.view().at(row,col) = 10 + float(row + window.row) * .5f;
                result.longitude.view().at(row,col) = 140 + float(col + window.col) * .5f;
                result.valid.view().at(row,col) = 1;
            }
        return result;
    }
    Shape shape() const { return image_ ? Shape{256,384} : Shape{4,6}; }
private:
    bool image_;
    std::shared_ptr<Counters> counters_;
    ProfileReport report_;
};

fy::products::ProductDefinition novel_product(const SensorMetadata& metadata,std::span<const BandDescriptor> bands,
                                               const fy::products::ProductRequest& request,bool image) {
    require(metadata.sensor == "NOVEL-INSTRUMENT" && request.mode == "preview");
    fy::products::ProductDefinition result;
    result.plan.mode = request.mode; result.plan.calculation_grid = "observations";
    result.plan.inputs.push_back({73,"observations"});
    result.plan.frames.push_back({.name = "preview",.units = {"test-unit"},.bit_depth = image ? 16 : 8});
    result.plan.validate(metadata,bands);
    result.prepare = [labels = metadata.labels](std::vector<ScalarPlane>&& inputs,
                           std::shared_ptr<const NativeGeometry> geometry,ExecutionContext& context) {
        auto phase = context.profile().phase("product");
        require(inputs.size() == 1 && inputs.front().unit == "test-unit");
        require(!inputs.front().valid.values().front() && std::isnan(inputs.front().values.values().front()));
        const auto values = inputs.front().values.values();
        for (std::size_t index = 1; index < values.size(); ++index)
            require(values[index] == float(10 + index % 47) * .5f + 7);
        fy::products::PreparedProduct product;
        product.frame.name = "preview"; product.frame.planes = std::move(inputs); product.frame.geometry = std::move(geometry);
        product.labels = labels;
        product.display.mapper = [](std::span<const float> values,const DisplayScale&) {
            return Color{values.front() / 50.,0,1 - values.front() / 50.};
        };
        product.frame.validate();
        return product;
    };
    return result;
}

}

int main(int argc,char** argv) {
    ScratchDirectory temporary;
    const auto directory = argc == 2 ? std::filesystem::path(argv[1]) : temporary.path();
    for (const bool image : {false,true}) {
        cv::Mat reference;
        for (const int threads : {1,4,0}) {
            ExecutionContext context({.threads = threads},"new_sensor");
            const auto counters = std::make_shared<Counters>();
            AdapterRegistry sensors;
            fy::products::ProductRegistry products;
            const auto key = image ? "novel-image" : "novel-swath";
            sensors.add(key,[image,counters,report = context.profile()](const SourceFiles&) {
                return std::make_unique<NovelAdapter>(image,counters,report);
            });
            products.add(key,[image](const auto& metadata,auto bands,const auto& request) {
                return novel_product(metadata,bands,request,image);
            });
            fy::Request request;
            request.inputs.push_back({"memory-scene",{},{}}); request.mode = "preview"; request.width = 256;
            request.output = directory / (std::string(key) + "-t" + std::to_string(threads) + ".png");
            fy::render_registered_scene(request,key,sensors,products,context);
            require(counters->serial && counters->worker != std::this_thread::get_id());
            require(counters->opened == 1 && counters->closed == 1 && counters->windows == 1 && counters->geometries == 1);
            const auto shape = image ? Shape{256,384} : Shape{4,6};
            require(counters->samples == shape.elements());
            const auto pixels = cv::imread(request.output.string(),cv::IMREAD_UNCHANGED);
            require(pixels.type() == (image ? CV_16UC3 : CV_8UC3));
            require(pixels.cols == (image ? 323 : 276) && pixels.rows == (image ? 386 : 175));
            const int center_row = image ? 108 + 85 : 11 + 76;
            if (image) {
                const auto center = pixels.at<cv::Vec<std::uint16_t,3>>(center_row,128);
                require(center[0] > 255 && center[1] == 0 && center[2] > 255);
            } else {
                const auto center = pixels.at<cv::Vec3b>(center_row,128);
                require(center[0] > 20 && center[1] == 0 && center[2] > 20);
            }
            if (reference.empty()) reference = pixels;
            else require(cv::norm(reference,pixels,cv::NORM_INF) == 0);
            context.profile().set_scratch_bytes(context.scratch().bytes());
            context.profile().finish(true);
            auto report_path = request.output; report_path += ".profile.json";
            context.profile().write_json(report_path);
            const auto report = context.profile().snapshot();
            require(report.source_pixels == shape.elements() && report.phases.at("product").calls == 1);
            for (const auto& buffer : *report.buffers) require(buffer.current_bytes == 0);
        }
    }
    std::cout << "Novel adapter and product registration rendered swath/RGB8 and image/RGB16 with identical 1/4/auto pixels\n";
}
