/// SPDX-License-Identifier: MIT

#include "products/swath.hpp"
#include "common.hpp"
#include "products/palette.hpp"
#include "products/phys.hpp"
#include "sensors/hdf.hpp"

#include <algorithm>
#include <cmath>

namespace fy::products {
using namespace core;
using namespace detail;

float percentile(std::span<const float> values, float percent, ExecutionContext& context) {
    if (!std::isfinite(percent) || percent < 0 || percent > 100) throw std::invalid_argument("Invalid percentile");
    auto reservation = context.profile().reserve(BufferKind::statistics,values.size() * sizeof(float));
    std::vector<float> samples;
    samples.reserve(values.size());
    for (float value : values) if (!std::isnan(value)) samples.push_back(value);
    if (samples.empty()) return 0;
    const auto rank = std::min(samples.size() - 1,static_cast<std::size_t>(percent / 100.0f * (samples.size() - 1)));
    std::nth_element(samples.begin(),samples.begin() + static_cast<std::ptrdiff_t>(rank),samples.end());
    return samples[rank];
}

ProductDefinition mersi_product(const SensorMetadata& metadata, std::span<const BandDescriptor> bands,
                           const ProductRequest& request) {
    const auto& mode = request.mode;
    const auto& satellite = metadata.satellite;
    const bool quarter = metadata.resolution == "0250M";
    const bool auxiliary = std::ranges::find(metadata.grids,std::string("auxiliary"),&GridDescriptor::id) != metadata.grids.end();
    ProductDefinition result;
    auto& plan = result.plan;
    plan.mode = mode; plan.calculation_grid = "native";
    std::vector<int> ids;
    std::vector<std::string> units;
    std::size_t infrared_index = 2, second_index = 3, zenith_index = 4, azimuth_index = 5;
    if (mode == "E" || mode == "R" || mode == "RMUL") {
        ids = {request.band};
        if (mode != "E") {
            if (mode == "RMUL" && satellite == "FY-3E") throw std::invalid_argument("RMUL is not supported for FY-3E");
            ids.push_back(sensors::solar_zenith);
        }
        units = {mode == "E" ? "degC" : "1"};
    } else if (mode == "LL") {
        allowed(satellite,{"FY-3E"}); ids = {sensors::low_light}; units = {"radiance"};
    } else if (mode == "TSG") units = {"degree"};
    else if (mode == "RT") {
        allowed(satellite,{"FY-3D","FY-3F","FY-3H"});
        ids = {1,2,3,sensors::solar_zenith}; units = {"1","1","1"};
    } else if (mode == "RY" || mode == "RF") {
        allowed(satellite,{"FY-3D","FY-3F","FY-3H","FY-3G"});
        const int red = satellite == "FY-3G" ? 1 : 3;
        ids = {red,red + 1,mode == "RY" ? (satellite == "FY-3G" ? 5 : 6) : (satellite == "FY-3G" ? 7 : 24),sensors::solar_zenith};
        units = {"1","1",mode == "RY" ? "1" : "degC"};
    } else if (mode == "EC" || mode == "EK") {
        allowed(satellite,mode == "EC" ? std::initializer_list<std::string_view>{"FY-3D","FY-3F","FY-3H","FY-3E","FY-3G"} :
                                           std::initializer_list<std::string_view>{"FY-3D","FY-3F","FY-3H","FY-3E"});
        if (quarter) throw std::invalid_argument("EC/EK require the native non-250M primary file");
        const int middle = satellite == "FY-3E" ? 2 : satellite == "FY-3G" ? 6 : 20;
        const int infrared = satellite == "FY-3E" ? 6 : satellite == "FY-3G" ? 7 : 24;
        if (mode == "EC") {
            int reference = (satellite == "FY-3G" || satellite == "FY-3F" || satellite == "FY-3H") ? infrared + 1 : infrared - 1;
            if (auxiliary) {
                plan.calculation_grid = "auxiliary";
                if (std::ranges::find(bands,reference + sensors::auxiliary_band_offset,&BandDescriptor::id) != bands.end())
                    reference += sensors::auxiliary_band_offset;
            }
            ids = {middle,reference,infrared + (auxiliary ? sensors::auxiliary_band_offset : 0),
                   infrared + 1 + (auxiliary ? sensors::auxiliary_band_offset : 0),sensors::solar_zenith,sensors::solar_azimuth};
            if (ids[1] == ids[3]) {
                ids.erase(ids.begin() + 3);
                second_index = 1; zenith_index = 3; azimuth_index = 4;
            }
            units = {"1","1","1"};
        } else {
            ids = {middle,infrared,infrared + 1,sensors::solar_zenith,sensors::solar_azimuth}; units = {"1"};
            infrared_index = 1; second_index = 2; zenith_index = 3; azimuth_index = 4;
        }
    } else throw std::invalid_argument("Unknown MERSI product: " + mode);
    if (mode == "R" || mode == "RMUL" || mode == "RT" || mode == "RY" || mode == "RF")
        if (!metadata.numeric_attributes.contains("earth_sun_distance_au"))
            throw std::invalid_argument("Reflection product needs EarthSun Distance Ratio");
    for (int id : ids) {
        band_at(bands,id);
        plan.inputs.push_back({id,plan.calculation_grid,mode == "EC" || mode == "EK" ? 10u : 0u,
                               mode == "EC" || mode == "EK" ? 10u : 0u,false,mode == "EC" || mode == "EK" || mode == "RF"});
    }
    plan.frames.push_back({.name = mode,.units = units});
    auto statistic = [&](StatisticsOperation operation,std::string field,float percentile = 99) -> StatisticsSpec& {
        StatisticsSpec spec;
        spec.operation = operation; spec.field = std::move(field); spec.percentile = percentile;
        spec.include_masked_finite = true;
        plan.statistics.push_back(std::move(spec));
        return plan.statistics.back();
    };
    if (mode == "E") statistic(StatisticsOperation::minmax,"temperature").domain = request.mosaic ? StatisticsDomain::task : StatisticsDomain::scene;
    if (mode == "LL") {
        statistic(StatisticsOperation::minmax,"radiance"); statistic(StatisticsOperation::percentile,"radiance",99);
    }
    if (mode == "RT" || mode == "RY" || mode == "RMUL") {
        auto& spec = statistic(StatisticsOperation::percentile,"reflection:0",99);
        if (request.mosaic) {
            spec.domain = StatisticsDomain::task; spec.rank = RankRule::histogram_ceiling_rank;
            spec.exclude_nan_only = false; spec.histogram_range = std::array<double,2>{0,8}; spec.histogram_bins = 65536;
        }
    }
    if (mode == "RF") {
        auto& warm = statistic(StatisticsOperation::top_mean,"background_temperature");
        warm.upper_cutoff = 30; warm.sample_divisor = 96; warm.ordered = true;
        statistic(StatisticsOperation::mean,"background_ratio").ordered = true;
    }
    if (mode == "EC" || mode == "EK") {
        statistic(StatisticsOperation::mean,"folded_solar_zenith").ordered = true;
        statistic(StatisticsOperation::mean,"folded_solar_azimuth").ordered = true;
    }
    if (mode == "EC") statistic(StatisticsOperation::percentile,"composite_max",98);
    if (mode == "EK") {
        statistic(StatisticsOperation::percentile,"middle_infrared",98);
        statistic(StatisticsOperation::percentile,"longwave_infrared",98);
        statistic(StatisticsOperation::percentile,"visible_black",3);
        statistic(StatisticsOperation::percentile,"black_corrected_visible",96);
    }
    plan.validate(metadata,bands);
    const auto description = ids.empty() || mode == "RT" || mode == "RF" ? std::string() : band_at(bands,ids.front()).name;
    const auto count = ids.size();
    result.prepare = [metadata,request,description,count,infrared_index,second_index,zenith_index,azimuth_index](std::vector<ScalarPlane>&& inputs,
                    std::shared_ptr<const NativeGeometry> geometry,ExecutionContext& context) {
        if (!geometry) throw std::invalid_argument("Missing product geometry");
        const auto shape = geometry_shape(*geometry);
        validate_planes(inputs,count,shape);
        auto phase = context.profile().phase("product");
        PreparedProduct output;
        output.frame.name = request.mode; output.frame.geometry = geometry;
        output.labels = labels(metadata,description,true);
        const auto& mode = request.mode;
        if (mode == "TSG") {
            const auto& positions = std::get<SwathGeometry>(*geometry);
            ScalarPlane plane{"degree",Raster<float>(shape),Raster<std::uint8_t>(shape)};
            context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                    auto projected = request.projection.project(positions.latitude.values()[i],positions.longitude.values()[i]);
                    const auto value = request.projection.orthographic ? projected.y / Deg2rad : positions.latitude.values()[i];
                    plane.valid.values()[i] = positions.valid.values()[i] && projected.valid;
                    plane.values.values()[i] = plane.valid.values()[i] ? float(value) : NAN;
                }
            });
            inputs.push_back(std::move(plane));
            context.profile().record_computed(shape.elements());
            output.display = scalar_display("",palette::color_plasma,{-90,90});
            output.labels.assign(5,"");
        } else if (mode == "E") {
            celsius(inputs[0],context);
            output.display = scalar_display(request.color,palette::color_rbtop_wv,
                                            request.mosaic ? DisplayScale{-105,40} : extrema(inputs[0],-105,40));
        } else if (mode == "LL") {
            auto scale = extrema(inputs[0],-INFINITY,INFINITY);
            scale.high = percentile(inputs[0].values.values(),99,context);
            if (scale.high == scale.low) scale.high = scale.low + 1;
            output.display = scalar_display("",palette::color_refsb,scale);
        } else if (mode == "R" || mode == "RMUL" || mode == "RT" || mode == "RY" || mode == "RF") {
            auto solar = std::move(inputs.back()); inputs.pop_back();
            const auto reflection_count = mode == "RF" ? 2 : inputs.size();
            for (std::size_t i = 0; i < reflection_count; ++i)
                solar_correct(inputs[i],solar,metadata.numeric_attributes.at("earth_sun_distance_au"),context);
            if (mode == "R") output.display = scalar_display(request.color,palette::color_enhanced_vis,{0,2});
            else if (mode == "RF") {
                celsius(inputs[2],context);
                auto reservation = context.profile().reserve(BufferKind::statistics,shape.elements() * sizeof(float));
                std::vector<float> candidates;
                candidates.reserve(shape.elements());
                for (auto value : inputs[2].values.values()) if (std::isfinite(value) && value <= 30) candidates.push_back(value);
                if (candidates.empty()) throw std::invalid_argument("RF has no background temperature samples at or below 30C");
                const auto samples = std::min(candidates.size(),std::max<std::size_t>(1,shape.elements() / 96));
                std::partial_sort(candidates.begin(),candidates.begin() + static_cast<std::ptrdiff_t>(samples),candidates.end(),std::greater<>{});
                float warm = 0;
                for (std::size_t i = 0; i < samples; ++i) warm += candidates[i];
                warm /= samples;
                float ratio = 0, count = 0;
                for (std::size_t i = 0; i < shape.elements(); ++i)
                    if (!std::isnan(inputs[0].values.values()[i]) && !std::isnan(inputs[2].values.values()[i])) {
                        ratio += (warm - inputs[2].values.values()[i]) / inputs[0].values.values()[i]; ++count;
                    }
                ratio /= count;
                if (!std::isfinite(ratio) || ratio == 0) throw std::invalid_argument("RF background ratio is undefined");
                output.display.mapper = [warm,ratio](std::span<const float> values,const DisplayScale&) {
                    const float r = values[0], sir = values[1];
                    float red = r * r;
                    red = std::max<float>(0,1 - std::sqrt(r) + r) * red + std::max<float>(0,std::sqrt(r) - r) * r;
                    return rgb(red,float(std::cosh(1.3 * sir) - 1),(warm - values[2]) / ratio);
                };
            } else {
                const auto scale = request.mosaic ? 1.0f : std::max(0.001f,percentile(inputs[0].values.values(),99,context));
                output.display.scale = {0,scale};
                output.shared_percentile_plane = 0;
                if (mode == "RMUL") output.display.mapper = [](std::span<const float> values,const DisplayScale& scale) {
                    const float value = values[0] / float(scale.high);
                    return rgb(float(std::pow(value,3)),float(value - 0.05),float(std::sqrt(std::max(0.0,double(value) - 0.1))));
                };
                else output.display.mapper = [](std::span<const float> values,const DisplayScale& scale) {
                    return rgb(values[2] / float(scale.high),values[1] / float(scale.high),values[0] / float(scale.high));
                };
            }
        } else {
            for (std::size_t i = 0; i < zenith_index; ++i) celsius(inputs[i],context);
            auto workspace = context.profile().reserve(BufferKind::statistics,shape.elements() * 2 * (sizeof(float) + sizeof(std::uint8_t)));
            auto shade = shadow(inputs[infrared_index],inputs[zenith_index],inputs[azimuth_index],context);
            if (mode == "EC") {
                ScalarPlane maximum{"1",Raster<float>(shape),Raster<std::uint8_t>(shape)};
                context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                    for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                        const float middle = inputs[0].values.values()[i], reference = inputs[1].values.values()[i];
                        const float infrared = inputs[infrared_index].values.values()[i], second = inputs[second_index].values.values()[i];
                        const float coefficient = float(0.5 * std::tanh(infrared * 0.1 + 5) + 0.5);
                        float red = (25 - reference) / 115, green = (25 - infrared) / 115;
                        float blue = (25 - (coefficient * middle + (1 - coefficient) * second)) / 115;
                        blue = float(0.36 * std::cos(9 * blue - 1.5) + 0.6 * blue + 0.12);
                        const float gain = -shade.values.values()[i];
                        const float shadow = float(std::pow((red + green + blue) / 3,3)) * gain;
                        red += shadow; green += shadow; blue += shadow;
                        const bool valid = shade.valid.values()[i] && inputs[0].valid.values()[i] && inputs[1].valid.values()[i] &&
                                           inputs[infrared_index].valid.values()[i] && inputs[second_index].valid.values()[i];
                        inputs[0].values.values()[i] = blue;
                        inputs[1].values.values()[i] = green;
                        inputs[2].values.values()[i] = red;
                        for (std::size_t channel = 0; channel < 3; ++channel) inputs[channel].valid.values()[i] = valid;
                        maximum.values.values()[i] = std::max({red,green,blue});
                        maximum.valid.values()[i] = valid;
                    }
                });
                const float divisor = std::max(1.0f,percentile(maximum.values.values(),98,context));
                inputs.resize(3);
                for (auto& plane : inputs) { transform(plane,context,[divisor](float value,std::size_t) { return value / divisor; }); plane.unit = "1"; }
                output.display.mapper = [](std::span<const float> values,const DisplayScale&) { return rgb(values[2],values[1],values[0]); };
            } else {
                const float middle_high = percentile(inputs[0].values.values(),98,context) + 3;
                const float infrared_high = percentile(inputs[infrared_index].values.values(),98,context) + 4;
                ScalarPlane visible{"1",Raster<float>(shape),Raster<std::uint8_t>(shape)};
                context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                    for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                        const float infrared = inputs[infrared_index].values.values()[i];
                        float coefficient = float((std::tanh(0.1 * infrared + 4) + 1) * 0.5);
                        const float middle = inputs[0].values.values()[i] * coefficient + inputs[second_index].values.values()[i] * (1 - coefficient);
                        const float subtraction = std::sqrt(std::max<float>((middle_high - middle) / (infrared_high - infrared) * 0.5,0));
                        coefficient = float((std::tanh(0.1 * infrared + 5) + 1) * 0.5);
                        float value = coefficient * subtraction + (1 - coefficient) * ((25 - inputs[second_index].values.values()[i]) / 115);
                        value += float(std::pow(value,3)) * -shade.values.values()[i];
                        visible.valid.values()[i] = shade.valid.values()[i] && inputs[0].valid.values()[i] &&
                            inputs[infrared_index].valid.values()[i] && inputs[second_index].valid.values()[i] && std::isfinite(value);
                        visible.values.values()[i] = std::isfinite(value) ? value : NAN;
                    }
                });
                const float black = percentile(visible.values.values(),3,context);
                transform(visible,context,[black](float value,std::size_t) { return value - black; });
                const float divisor = std::max(0.001f,percentile(visible.values.values(),96,context));
                transform(visible,context,[divisor](float value,std::size_t) { return value / divisor; });
                inputs.clear(); inputs.push_back(std::move(visible));
                output.display.mapper = [](std::span<const float> value,const DisplayScale&) { return rgb(value[0],value[0],value[0]); };
            }
        }
        output.frame.planes = std::move(inputs);
        output.frame.validate();
        return output;
    };
    return result;
}

ProductDefinition mwri_product(const SensorMetadata& metadata, std::span<const BandDescriptor> bands,
                          const ProductRequest& request) {
    const auto& mode = request.mode;
    if (metadata.satellite == "FY-3D" && (mode == "S" || mode == "TCN" || mode == "TCN2"))
        throw std::invalid_argument("Requested MWRI product requires sounding channels");
    std::vector<int> ids;
    std::string description;
    if (mode == "W" || mode == "S") ids = {request.band + (mode == "S" ? sensors::sounding_band_offset : 0)};
    else if (mode == "W84") ids = {8,9};
    else if (mode == "TCX") { ids = {8,9,6}; description = "TCX: PCT89 / Pol / Scat"; }
    else if (mode == "TCN") { ids = {111,110,108}; description = "TCN: 118GHz Warm Core"; }
    else if (mode == "TCN2") { ids = {106,111,108}; description = "TCN2: 53.75V / 118±1.2 / 118±3.2"; }
    else if (mode == "TCS") { ids = {8,6,4,2}; description = "TCS: 89-36 / 36-18 / 23-18"; }
    else if (mode == "TCR") { ids = {8,9,4,6}; description = "TCR: PCT89 / 23V / 36V"; }
    else throw std::invalid_argument("Unknown MWRI product: " + mode);
    ProductDefinition result;
    result.plan.regional_default_half_range = 7.5;
    result.plan.mode = mode;
    result.plan.calculation_grid = band_at(bands,ids.front()).grid;
    for (int id : ids) {
        const auto& band = band_at(bands,id);
        if (band.grid != result.plan.calculation_grid || band.calibrated_unit != "K")
            throw std::invalid_argument("MWRI product channels use different grids or units");
        result.plan.inputs.push_back({id,result.plan.calculation_grid});
    }
    result.plan.frames.push_back({.name = mode,.units = std::vector<std::string>(ids.size(),"K")});
    if (mode == "W" || mode == "S") {
        StatisticsSpec spec;
        spec.operation = StatisticsOperation::minmax; spec.field = "brightness_temperature"; spec.before_crop = false;
        spec.include_masked_finite = true;
        result.plan.statistics.push_back(std::move(spec));
    }
    result.plan.validate(metadata,bands);
    if (description.empty()) description = band_at(bands,ids.front()).name;
    if (mode == "S" || mode == "W") {
        description += " #" + std::to_string(request.band);
        if (metadata.satellite == "FY-3D") {
            constexpr std::string_view names[]{"10.65V","10.65H","18.7V","18.7H","23.8V","23.8H","36.5V","36.5H","89V","89H"};
            if (request.band >= 0 && request.band < 10) description += " (" + std::string(names[request.band]) + ")";
        }
    }
    result.prepare = [metadata,mode,description,count = ids.size()](std::vector<ScalarPlane>&& inputs,
                    std::shared_ptr<const NativeGeometry> geometry,ExecutionContext&) {
        if (!geometry) throw std::invalid_argument("Missing MWRI geometry");
        validate_planes(inputs,count,geometry_shape(*geometry));
        PreparedProduct result;
        result.frame.name = mode; result.frame.geometry = std::move(geometry);
        result.labels = labels(metadata,description,false);
        if (mode == "W" || mode == "S") result.display = scalar_display("",palette::color_plasma,extrema(inputs[0],-INFINITY,INFINITY));
        else result.display.mapper = [mode](std::span<const float> v,const DisplayScale&) {
            if (mode == "W84") {
                const float pct = float(1.818 * v[0] - 0.818 * v[1]);
                return rgb((300-pct)*1.5f,(300-v[0])*1.5f,(300-v[1])*1.5f,255);
            }
            if (mode == "TCX") {
                const float pct = float(1.818 * v[0] - 0.818 * v[1]);
                return rgb((300-pct)*1.5f,(v[0]-v[1])*5,float((v[0]-v[2]+80)*1.8),255);
            }
            if (mode == "TCN") return rgb((v[0]-200)*3,(v[1]-200)*3,(v[2]-240)*3,255);
            if (mode == "TCN2") return rgb((v[0]-210)*4,(v[1]-210)*4,(v[2]-240)*3,255);
            if (mode == "TCS") return rgb((v[0]-v[1]+50)*2.5f,(v[1]-v[3]+17)*5,(v[2]-v[3]+17)*5,255);
            const float pct = float(1.818 * v[0] - 0.818 * v[1]);
            return rgb((300-pct)*1.5f,(300-v[2])*1.5f,(300-v[3])*1.5f,255);
        };
        result.frame.planes = std::move(inputs);
        result.frame.validate();
        return result;
    };
    return result;
}

}
