/// SPDX-License-Identifier: MIT

#include "products/giirs.hpp"
#include "common.hpp"
#include "core/stream.hpp"
#include "sensors/hdf.hpp"

#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fy::products {
namespace {

using namespace core;
using namespace detail;

std::vector<std::string> giirs_labels(const SensorMetadata& metadata,const std::string& description) {
    auto result = labels(metadata,description,false);
    const auto separator = metadata.observation_begin.find('T');
    auto time = separator == std::string::npos ? std::string{} : metadata.observation_begin.substr(separator + 1);
    const auto fraction = time.rfind('.');
    time = fraction == std::string::npos ? "" : time.substr(0,fraction);
    result[2] = time + "(UTC)";
    return result;
}

std::string frame_name(std::size_t index) {
    std::ostringstream name;
    name << std::setfill('0') << std::setw(6) << index;
    return name.str();
}

DisplayScale finite_extrema(const ScalarPlane& plane,bool required = true) {
    DisplayScale scale{INFINITY,-INFINITY};
    for (const float value : plane.values.values())
        if (std::isfinite(value)) { scale.low = std::min(scale.low,double(value)); scale.high = std::max(scale.high,double(value)); }
    if (required && !std::isfinite(scale.low)) throw std::invalid_argument("GIIRS frame has no finite samples");
    return scale;
}

class SequenceCache {
public:
    SequenceCache(std::filesystem::path path,ProfileReport& report) : path_(std::move(path)),report_(report) {
        if (std::filesystem::exists(path_)) throw std::invalid_argument("GIIRS cache already exists");
    }
    SequenceCache(const SequenceCache&) = delete;
    SequenceCache& operator=(const SequenceCache&) = delete;
    ~SequenceCache() {
        std::error_code error;
        if (owned_) {
            std::filesystem::remove(path_,error);
            if (!error) report_.release_cache(bytes_);
        }
    }
    std::ofstream create() {
        std::ofstream output(path_,std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        owned_ = true;
        return output;
    }
    template<class T> void write(std::ofstream& output,std::span<const T> values) {
        output.write(reinterpret_cast<const char*>(values.data()),static_cast<std::streamsize>(values.size_bytes()));
        bytes_ += values.size_bytes(); report_.record_cache_write(values.size_bytes());
    }
    template<class T> void read(std::ifstream& input,std::span<T> values) {
        input.read(reinterpret_cast<char*>(values.data()),static_cast<std::streamsize>(values.size_bytes()));
        report_.record_cache_read(values.size_bytes());
    }
    std::ifstream open() const {
        if (!owned_ || std::filesystem::file_size(path_) != bytes_) throw std::runtime_error("GIIRS cache size changed");
        std::ifstream input(path_,std::ios::binary);
        input.exceptions(std::ios::badbit | std::ios::failbit);
        return input;
    }
private:
    std::filesystem::path path_;
    ProfileReport& report_;
    std::uint64_t bytes_ = 0;
    bool owned_ = false;
};

}

ProductDefinition giirs_product(const SensorMetadata& metadata,std::span<const BandDescriptor> bands,
                               const ProductRequest& request,bool sequence_calibration) {
    if (metadata.sensor != "GIIRS" || (request.mode != "R" && request.mode != "LW" && request.mode != "MW"))
        throw std::invalid_argument("Unknown GIIRS product: " + request.mode);
    const bool visible = request.mode == "R";
    if (request.band < 0 || request.band >= sensors::giirs_lw_offset)
        throw std::invalid_argument("Invalid GIIRS spectral channel");
    const int id = visible ? 1 : request.band + (request.mode == "LW" ? sensors::giirs_lw_offset : sensors::giirs_mw_offset);
    const auto& band = band_at(bands,id);
    const auto grid = band.grid;
    ProductDefinition definition;
    auto& plan = definition.plan;
    plan.mode = request.mode; plan.calculation_grid = grid;
    plan.inputs.push_back({.band = id,.target_grid = grid,.raw_samples = !visible && !sequence_calibration});
    if (visible)
        for (int coefficient = 2; coefficient <= 4; ++coefficient) plan.inputs.push_back({coefficient,grid});
    plan.frames.push_back({.name = request.mode,.units = {visible ? "1" : "K"},
                          .spectral_channel = visible ? std::nullopt : std::optional<std::size_t>(request.band)});
    StatisticsSpec statistics;
    statistics.operation = visible ? StatisticsOperation::percentile : StatisticsOperation::minmax;
    statistics.field = visible ? "reflectance" : "brightness_temperature";
    statistics.before_crop = true; statistics.include_masked_finite = true;
    plan.statistics.push_back(statistics);
    plan.validate(metadata,bands);
    const float wavenumber = visible ? 0 : static_cast<float>(metadata.numeric_attributes.at(
                                                "wavenumber." + request.mode + "." + std::to_string(request.band)));
    const auto description = band.name + (visible ? "" : " " + std::to_string(wavenumber) + "cm^-1");
    auto frame_labels = giirs_labels(metadata,description);
    definition.prepare = [visible,wavenumber,sequence_calibration,mode = request.mode,frame_labels = std::move(frame_labels)](
            std::vector<ScalarPlane>&& inputs,std::shared_ptr<const NativeGeometry> geometry,ExecutionContext& context) {
        auto phase = context.profile().phase("product");
        if (!geometry || !std::holds_alternative<SwathGeometry>(*geometry))
            throw std::invalid_argument("GIIRS requires its observation grid");
        validate_planes(inputs,visible ? 4 : 1,geometry_shape(*geometry));
        auto& plane = inputs.front();
        if (visible) transform(plane,context,[&](float dn,std::size_t index) {
            plane.valid.values()[index] = plane.valid.values()[index] && inputs[1].valid.values()[index] &&
                                          inputs[2].valid.values()[index] && inputs[3].valid.values()[index];
            return dn * dn * inputs[1].values.values()[index] + dn * inputs[2].values.values()[index] +
                   inputs[3].values.values()[index];
        });
        else transform(plane,context,[wavenumber](float radiance,std::size_t) { return static_cast<float>(brtemp(radiance,wavenumber)); });
        plane.unit = visible ? "1" : "K";
        PreparedProduct product;
        product.frame.name = mode; product.frame.geometry = std::move(geometry);
        product.display = scalar_display("",visible ? palette::color_refsb : palette::color_plasma,
                             visible ? DisplayScale{0,percentile(plane.values.values(),99,context)} : finite_extrema(plane,!sequence_calibration));
        product.labels = frame_labels;
        product.frame.planes.push_back(std::move(plane));
        product.frame.validate();
        return product;
    };
    return definition;
}

ProductPlan giirs_sequence_plan(const SensorMetadata& metadata,std::span<const BandDescriptor> bands) {
    if (metadata.sensor != "GIIRS") throw std::invalid_argument("IRall requires GIIRS");
    ProductPlan plan;
    plan.mode = "IRall"; plan.calculation_grid = "MW"; plan.stream_frames = true;
    for (const auto* family : {"MW","LW"}) {
        std::vector<const BandDescriptor*> channels;
        for (const auto& band : bands) if (band.grid == family) channels.push_back(&band);
        std::ranges::sort(channels,{},[](const auto* band) { return band->channel_index; });
        if (channels.empty()) throw std::invalid_argument("IRall is missing a spectral family");
        for (const auto* channel : channels) {
            plan.inputs.push_back({channel->id,family});
            plan.frames.push_back({.name = frame_name(plan.frames.size()),.units = {"K"},
                                   .spectral_channel = channel->channel_index,.grid = family});
        }
        StatisticsSpec statistics;
        statistics.domain = StatisticsDomain::frame_sequence; statistics.field = family;
        statistics.operation = StatisticsOperation::minmax; statistics.include_masked_finite = true;
        plan.statistics.push_back(statistics);
    }
    plan.validate(metadata,bands);
    return plan;
}

void stream_giirs_sequence(SensorSession& session,ExecutionContext& context,
                          const std::function<void(const PreparedProduct&,const FramePlan&)>& consume) {
    if (!consume) throw std::invalid_argument("GIIRS sequence needs a consumer");
    const auto metadata = session.metadata();
    const auto bands = session.bands();
    const auto plan = giirs_sequence_plan(metadata,bands);
    std::uint64_t samples = 0;
    for (const auto* family : {"MW","LW"}) {
        const auto shape = metadata.grid(family).shape;
        auto geometry = std::make_shared<NativeGeometry>(session.read_geometry(family,{0,0,shape}));
        auto geometry_memory = context.profile().reserve(BufferKind::scene,shape.elements() * 9);
        SequenceCache cache(context.scratch().path() / (std::string("giirs-") + family + ".frames"),context.profile());
        auto output = cache.create();
        std::vector<std::pair<PreparedProduct,FramePlan>> descriptions;
        DisplayScale shared{INFINITY,-INFINITY};
        for (std::size_t index = 0; index < plan.frames.size(); ++index) {
            const auto& frame = plan.frames[index];
            if (frame.grid != family) continue;
            const auto& dependency = plan.inputs[index];
            auto memory = context.profile().reserve(BufferKind::scene,shape.elements() * 5);
            ScalarPlane plane{band_at(bands,dependency.band).calibrated_unit,Raster<float>(shape),Raster<std::uint8_t>(shape)};
            stream_calibrated(session,dependency.band,{0,0,shape},context,[&](SampleTile&& tile) {
                const auto offset = tile.samples.window.row * shape.cols;
                std::memcpy(plane.values.values().data() + offset,tile.samples.values.values().data(),tile.samples.values.bytes());
                std::memcpy(plane.valid.values().data() + offset,tile.samples.valid.values().data(),tile.samples.valid.bytes());
            },256,InvalidSamplePolicy::retain_finite);
            auto definition = giirs_product(metadata,bands,{.mode = family,.band = static_cast<int>(*frame.spectral_channel)},true);
            std::vector<ScalarPlane> inputs;
            inputs.push_back(std::move(plane));
            auto product = definition.prepare(std::move(inputs),geometry,context);
            shared.low = std::min(shared.low,product.display.scale.low); shared.high = std::max(shared.high,product.display.scale.high);
            auto phase = context.profile().phase("sequence_cache_write");
            cache.write<float>(output,product.frame.planes.front().values.values());
            cache.write<std::uint8_t>(output,product.frame.planes.front().valid.values());
            product.frame.planes.clear();
            descriptions.emplace_back(std::move(product),frame);
            samples += shape.elements();
            context.profile().record_cached_scene();
        }
        output.close();
        if (!std::isfinite(shared.low)) throw std::invalid_argument("GIIRS spectral family has no finite samples");
        auto input = cache.open();
        for (auto& [product,frame] : descriptions) {
            auto memory = context.profile().reserve(BufferKind::scene,shape.elements() * 5);
            ScalarPlane plane{"K",Raster<float>(shape),Raster<std::uint8_t>(shape)};
            {
                auto phase = context.profile().phase("sequence_cache_read");
                cache.read(input,plane.values.values()); cache.read(input,plane.valid.values());
            }
            product.frame.name = frame.name; product.frame.planes.push_back(std::move(plane));
            product.display.scale = shared;
            product.frame.validate();
            consume(product,frame);
            product.frame.planes.clear();
        }
    }
    context.profile().set_source_pixels(samples);
}

}
