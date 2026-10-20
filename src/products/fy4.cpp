/// SPDX-License-Identifier: MIT

#include "products/fy4.hpp"
#include "common.hpp"
#include "core/resample.hpp"
#include "sensors/hdf.hpp"

namespace fy::products {
namespace {

using namespace core;
using namespace detail;

float smoothstep(float low,float high,float value) {
    const auto x = std::clamp((value - low) / (high - low),0.0f,1.0f);
    return x * x * (3.0f - 2.0f * x);
}


void compose_rtec(std::vector<ScalarPlane>& inputs,const ScalarPlane& shade,ExecutionContext& context) {
    const auto shape = inputs[0].values.shape();
    const auto trans = [](float value) { return (25.0f - value) / 115.0f; };
    context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
        for (auto row = begin; row < end; ++row) {
            float latest = 20.0f;
            for (std::size_t col = 0; col < shape.cols; ++col) {
                const auto i = row * shape.cols + col;
                const float mwir = inputs[3].values.values()[i], refcl = inputs[4].values.values()[i];
                const float lir1 = inputs[5].values.values()[i], lir2 = inputs[6].values.values()[i], solar = inputs[7].values.values()[i];
                const bool night_valid = !std::isnan(lir1) && !std::isnan(lir2) && !std::isnan(mwir);
                std::array<float,3> night{};
                if (night_valid) {
                    const float update = 0.5f * std::tanh(0.1f * lir1 - 0.9f) + 0.5f;
                    latest = update * lir1 + (1.0f - update) * latest;
                    const float mwir_coefficient = 0.5f * std::tanh(lir1 * 0.1f + 5.0f) + 0.5f;
                    const float coefficient = (1.0f - trans(lir1)) * 0.5f * std::tanh(0.15f * latest - 2.0f) + 0.5f;
                    const float ldlb = (26.0f - mwir) / (29.0f - lir1);
                    const float fllb = trans(mwir_coefficient * mwir + (1.0f - mwir_coefficient) * (lir1 - 7.0f));
                    float b = coefficient * (mwir_coefficient * ldlb + (1.0f - mwir_coefficient) * trans(lir1)) +
                              (1.0f - coefficient) * (0.36f * std::cos(9.0f * fllb - 1.5f) + 0.6f * fllb);
                    float r = trans(refcl), g = trans(lir1);
                    const float low_cloud = smoothstep(0.5f,3.0f,lir1 - mwir) * smoothstep(-40.0f,-8.0f,lir1) * smoothstep(92.0f,98.0f,solar);
                    const float brightness = std::max({r,g,b}), weight = 0.28f * low_cloud;
                    b = b * (1.0f - weight) + std::min(brightness * 1.05f,1.0f) * weight;
                    g = g * (1.0f - weight) + brightness * weight; r = r * (1.0f - weight) + brightness * weight;
                    const float cdo = (1.0f - smoothstep(-50.0f,-35.0f,lir1)) *
                        (1.0f - smoothstep(1.0f,3.0f,std::abs(lir2 - lir1))) * smoothstep(90.0f,96.0f,solar);
                    const float cdo_brightness = std::max({r,g,b}), cdo_weight = 0.32f * cdo;
                    b = b * (1.0f - cdo_weight) + cdo_brightness * cdo_weight;
                    g = g * (1.0f - cdo_weight) + cdo_brightness * cdo_weight;
                    r = r * (1.0f - cdo_weight) + cdo_brightness * cdo_weight;
                    const float cirrus = smoothstep(0.3f,1.2f,refcl - lir1) * smoothstep(0.7f,2.5f,std::abs(lir1 - lir2)) *
                        (1.0f - smoothstep(-32.0f,-5.0f,lir1)) * (1.0f - cdo);
                    const float suppression = 0.85f * smoothstep(0.03f,0.18f,std::min(r,g) - b) * (1.0f - cirrus) * (1.0f - cdo);
                    const float neutral = (r + g + b) / 3.0f;
                    r = r * (1.0f - suppression) + neutral * suppression;
                    g = g * (1.0f - suppression) + neutral * suppression;
                    const float gain = -shade.values.values()[i] / 3.0f, average = (r + g + b) / 3.0f;
                    const float shadow = average * average * average * gain;
                    r += shadow; g += shadow; b += shadow;
                    night = {r,g,b};
                }
                bool night_mask = night_valid && shade.valid.values()[i] && inputs[7].valid.values()[i];
                for (std::size_t channel = 3; channel <= 6; ++channel) night_mask = night_mask && inputs[channel].valid.values()[i];
                const bool day_valid = solar < 90 && !std::isnan(inputs[0].values.values()[i]) &&
                    !std::isnan(inputs[1].values.values()[i]) && !std::isnan(inputs[2].values.values()[i]);
                const float night_weight = day_valid && night_valid ? smoothstep(82.0f,90.0f,solar) : 0;
                bool valid = day_valid ? inputs[0].valid.values()[i] && inputs[1].valid.values()[i] && inputs[2].valid.values()[i] &&
                                         (night_weight == 0 || night_mask) : night_mask;
                std::array<float,3> color = night;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    if (day_valid) color[channel] = std::clamp(inputs[channel].values.values()[i],0.0f,1.0f) * (1.0f - night_weight) + night[channel] * night_weight;
                    else if (!night_valid) color[channel] = 0;
                    valid = valid && std::isfinite(color[channel]);
                }
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    inputs[channel].values.values()[i] = color[channel]; inputs[channel].valid.values()[i] = valid;
                }
            }
        }
    });
    context.profile().record_computed(shape.elements());
}

ScalarPlane align(const ScalarPlane& plane,Shape shape,ExecutionContext& context) {
    const auto source = plane.values.shape();
    auto result = resample_cubic(plane.values.view(),plane.valid.view(),{0,0,source},source,{0,0,shape},shape,false,context);
    result.unit = plane.unit;
    return result;
}

}

ProductDefinition fy4_product(const SensorMetadata& metadata,std::span<const BandDescriptor> bands,const ProductRequest& request) {
    if (metadata.satellite.rfind("FY-4",0) != 0 || (metadata.sensor != "AGRI" && metadata.sensor != "GHI"))
        throw std::invalid_argument("FY4 product requires AGRI or GHI");
    const bool ghi = metadata.sensor == "GHI";
    const bool auxiliary = std::ranges::any_of(metadata.grids,[](const auto& grid) { return grid.id == "auxiliary"; });
    auto mode = request.mode;
    if (mode == "Re") mode = "R";
    if (mode == "RT+EC" || (mode == "AGHI_ALD" && !ghi)) mode = "RTEC";
    if (mode == "RT16") mode = "RTEC16";
    ProductDefinition result;
    auto& plan = result.plan;
    plan.mode = mode; plan.calculation_grid = "native";
    std::vector<int> ids;
    std::vector<std::string> units;
    const auto aux = [&](int band) { return band + (auxiliary ? sensors::auxiliary_band_offset : 0); };
    const bool rtec = mode == "RTEC" || mode == "RTEC16";
    if ((mode == "RTG" && !ghi) || ((rtec || mode == "EC") && ghi))
        throw std::invalid_argument("FY4 mode is not configured for sensor: " + metadata.sensor);
    if (mode == "E" || mode == "Rr") { ids = {request.band}; units = {mode == "E" ? "degC" : "1"}; }
    else if (mode == "R") { ids = {request.band,sensors::solar_zenith}; units = {"1"}; }
    else if (mode == "EC") { ids = {7,12,13,14,sensors::solar_zenith,sensors::solar_azimuth}; units = {"1","1","1"}; }
    else if (rtec) {
        allowed(metadata.satellite,{"FY-4A","FY-4B"});
        const int infrared = metadata.satellite == "FY-4A" ? 12 : 13;
        ids = {2,3,1,aux(7),aux(infrared - 1),aux(infrared),aux(infrared + 1),
               sensors::solar_zenith,sensors::solar_azimuth,sensors::satellite_zenith};
        units = {"1","1","1"};
    } else if (mode == "RF") { ids = {ghi ? 4 : 2,ghi ? 1 : 3,aux(ghi ? 7 : 13),sensors::solar_zenith}; units = {"1","1","degC"}; }
    else if (mode == "RY") { ids = {ghi ? 4 : 2,ghi ? 1 : 3,aux(ghi ? 6 : 5),sensors::solar_zenith}; units = {"1","1","1"}; }
    else if (mode == "RX") { ids = {ghi ? 4 : 2,ghi ? 1 : 6,ghi ? 6 : 5,sensors::solar_zenith}; units = {"1","1","1"}; }
    else if (mode == "RTpp") { ids = {2,3,1,sensors::solar_zenith}; units = {"1","1","1"}; }
    else if (mode == "AGHI_ALD") { ids = {4,3,2,7,sensors::solar_zenith}; units = {"1","1","1"}; }
    else if (mode == "RTG") {
        ids = {4,3,2,sensors::solar_zenith,sensors::satellite_zenith}; units = {"1","1","1"};
        if (auxiliary) ids.push_back(sensors::auxiliary_band_offset + 1);
    } else throw std::invalid_argument("Unknown FY4 product: " + mode);
    const bool reflection = mode != "E" && mode != "Rr" && mode != "EC";
    if (reflection && !metadata.numeric_attributes.contains("earth_sun_distance_au"))
        throw std::invalid_argument("FY4 reflection product needs Earth-Sun distance ratio");
    for (const int id : ids) {
        const auto& band = band_at(bands,id);
        const auto target = mode == "RTG" && id == sensors::solar_zenith ? band.grid :
                            mode == "RTG" && id == sensors::auxiliary_band_offset + 1 ? "auxiliary" : "native";
        plan.inputs.push_back({id,target,mode == "EC" || rtec ? 10u : 0u,mode == "EC" || rtec ? 10u : 0u,rtec,rtec || mode == "EC" || mode == "RF"});
    }
    plan.frames.push_back({.name = mode,.units = units,.bit_depth = mode == "RTEC16" ? 16 : 8,
                           .grid = mode == "RTG" && auxiliary ? "auxiliary" : "native"});
    auto statistic = [&](StatisticsOperation operation,const std::string& field,float percent = 99) {
        StatisticsSpec spec;
        spec.operation = operation; spec.field = field; spec.percentile = percent; spec.include_masked_finite = true;
        spec.ordered = operation == StatisticsOperation::mean;
        plan.statistics.push_back(spec);
    };
    if (mode == "Rr") statistic(StatisticsOperation::percentile,"reflection",99.9f);
    if (mode == "RF" || mode == "AGHI_ALD") statistic(StatisticsOperation::percentile,"temperature",95);
    if (mode == "RF") statistic(StatisticsOperation::mean,"background_ratio");
    if (mode == "EC" || rtec) {
        statistic(StatisticsOperation::mean,"folded_solar_zenith"); statistic(StatisticsOperation::mean,"folded_solar_azimuth");
    }
    if (mode == "EC") statistic(StatisticsOperation::percentile,"composite_max",98);
    plan.validate(metadata,bands);
    const auto description = band_at(bands,mode == "AGHI_ALD" ? 3 : ids.front()).name;
    const auto count = ids.size();
    std::vector<Shape> input_shapes;
    for (const auto& dependency : plan.inputs) input_shapes.push_back(metadata.grid(dependency.target_grid).shape);
    const auto output_shape = metadata.grid(plan.frames.front().grid).shape;
    result.prepare = [metadata,mode,description,count,rtec,auxiliary,input_shapes,output_shape](std::vector<ScalarPlane>&& inputs,
                      std::shared_ptr<const NativeGeometry> geometry,ExecutionContext& context) {
        const auto shape = metadata.grid("native").shape;
        if (!geometry || geometry_shape(*geometry) != output_shape || inputs.size() != count)
            throw std::invalid_argument("Invalid FY4 product inputs");
        for (std::size_t i = 0; i < inputs.size(); ++i)
            if (inputs[i].values.shape() != input_shapes[i] || inputs[i].valid.shape() != input_shapes[i])
                throw std::invalid_argument("FY4 input calculation grid mismatch");
        auto phase = context.profile().phase("product");
        PreparedProduct output;
        output.frame.name = mode; output.frame.geometry = geometry;
        output.labels = labels(metadata,mode == "E" || mode == "R" || mode == "Rr" ? description : "",true);
        const auto to_celsius = [&](ScalarPlane& plane) {
            transform(plane,context,[](float value,std::size_t) { return float(value + K2c); }); plane.unit = "degC";
        };
        const double distance = metadata.numeric_attributes.contains("earth_sun_distance_au") ? metadata.numeric_attributes.at("earth_sun_distance_au") : 1;
        if (mode == "E" || mode == "Rr" || mode == "R") {
            if (mode == "E") {
                to_celsius(inputs[0]); output.display = scalar_display("",palette::color_rbtop_wv,{-105,40});
            } else if (mode == "Rr") output.display = scalar_display("",palette::color_refsb,{0,std::clamp(percentile(inputs[0].values.values(),99.9f,context),1.0f,2.0f)});
            else { solar_correct(inputs[0],inputs[1],distance,context); output.display = scalar_display("",palette::color_enhanced_vis,{0,2}); }
            inputs.resize(1);
        } else if (rtec) {
            context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                for (std::size_t channel = 3; channel <= 6; ++channel) {
                    auto values = inputs[channel].values.values();
                    auto valid = inputs[channel].valid.values();
                    for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                        const float value = std::isfinite(values[i]) ? float(values[i] + K2c) : NAN;
                        valid[i] = valid[i] && std::isfinite(value);
                        values[i] = value;
                    }
                }
            });
            for (std::size_t channel = 3; channel <= 6; ++channel) inputs[channel].unit = "degC";
            context.profile().record_computed(shape.elements() * 4);
            auto memory = context.profile().reserve(BufferKind::statistics,shape.elements() * 5);
            auto shade = shadow(inputs[5],inputs[7],inputs[8],context);
            correct_agri_day(inputs[0],inputs[1],inputs[2],inputs[7],inputs[9],metadata.satellite,distance,context);
            compose_rtec(inputs,shade,context); inputs.resize(3);
        } else if (mode == "EC") {
            /// EC 沿用旧公式，第四个热红外输入保持 K。
            for (std::size_t i = 0; i < 3; ++i) to_celsius(inputs[i]);
            auto memory = context.profile().reserve(BufferKind::statistics,shape.elements() * 10);
            auto shade = shadow(inputs[2],inputs[4],inputs[5],context);
            ScalarPlane maximum{"1",Raster<float>(shape),Raster<std::uint8_t>(shape)};
            context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                    const float coefficient = float(0.7 * std::tanh(inputs[2].values.values()[i] * 0.1 + 5) + 0.5);
                    float b = (25 - (coefficient * inputs[0].values.values()[i] + (1 - coefficient) * inputs[3].values.values()[i])) / 115;
                    float r = (25 - inputs[1].values.values()[i]) / 115, g = (25 - inputs[2].values.values()[i]) / 115;
                    b = float(0.36 * std::cos(9 * b - 1.5) + 0.6 * b + 0.12);
                    const float gain = -shade.values.values()[i] / 3;
                    const float shade_coefficient = float(std::pow((r + g + b) / 3,3));
                    const float shadow_value = shade_coefficient * gain;
                    r += shadow_value; g += shadow_value; b += shadow_value;
                    bool valid = shade.valid.values()[i];
                    for (std::size_t channel = 0; channel < 4; ++channel) valid = valid && inputs[channel].valid.values()[i];
                    inputs[0].values.values()[i] = r; inputs[1].values.values()[i] = g; inputs[2].values.values()[i] = b;
                    for (std::size_t channel = 0; channel < 3; ++channel) inputs[channel].valid.values()[i] = valid && std::isfinite(inputs[channel].values.values()[i]);
                    maximum.values.values()[i] = std::max({r,b,g}); maximum.valid.values()[i] = valid;
                }
            });
            context.profile().record_computed(shape.elements());
            const float divisor = std::max(percentile(maximum.values.values(),98,context),1.0f);
            inputs.resize(3);
            for (auto& plane : inputs) transform(plane,context,[divisor](float value,std::size_t) { return value / divisor; });
        } else if (mode == "RTG") {
            auto workspace = context.profile().reserve(BufferKind::statistics,shape.elements() * 5);
            auto solar = align(inputs[3],shape,context);
            for (std::size_t i = 0; i < 3; ++i) solar_correct(inputs[i],solar,distance,context);
            context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                constexpr std::array coefficients{0.0496f,0.1006f,0.1858f};
                for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                    const float mu_s = std::cos(solar.values.values()[i] * float(Deg2rad));
                    const float mu_v = std::cos(inputs[4].values.values()[i] * float(Deg2rad));
                    const float weight = 1.0f - std::clamp(std::max({inputs[2].values.values()[i],inputs[1].values.values()[i],inputs[0].values.values()[i]}) / 0.6f,0.0f,1.0f);
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const float tau = coefficients[channel];
                        const float path = (1.0f - std::exp(-tau / mu_s - tau / mu_v)) / (2.0f * (1.0f / mu_s + 1.0f / mu_v));
                        const float clean = std::max(inputs[channel].values.values()[i] - path * weight,0.0f);
                        inputs[channel].values.values()[i] = float(std::pow(clean,channel == 0 ? 0.85 : 0.8) * (channel == 0 ? 0.979 : channel == 1 ? 1.097 : 1.19));
                        inputs[channel].valid.values()[i] = inputs[channel].valid.values()[i] && solar.valid.values()[i] && inputs[4].valid.values()[i] && std::isfinite(inputs[channel].values.values()[i]);
                    }
                }
            });
            context.profile().record_computed(shape.elements());
            if (auxiliary) {
                const auto target_shape = geometry_shape(*geometry);
                workspace.resize(shape.elements() * 5 + target_shape.elements() * 20);
                auto target_solar = align(inputs[3],target_shape,context);
                solar_correct(inputs[5],target_solar,distance,context);
                std::vector<ScalarPlane> channels;
                for (std::size_t i = 0; i < 3; ++i) channels.push_back(align(inputs[i],target_shape,context));
                context.parallel_rows(target_shape.rows,[&](std::size_t begin,std::size_t end) {
                    for (auto i = begin * target_shape.cols; i < end * target_shape.cols; ++i) {
                        const float r = channels[0].values.values()[i], g = channels[1].values.values()[i], b = channels[2].values.values()[i];
                        const float coefficient = 3 * inputs[5].values.values()[i] / (b + g + r);
                        const bool valid = channels[0].valid.values()[i] && channels[1].valid.values()[i] && channels[2].valid.values()[i] &&
                                           inputs[5].valid.values()[i] && b >= 1e-3 && std::isfinite(coefficient);
                        for (auto& plane : channels) { plane.values.values()[i] *= coefficient; plane.valid.values()[i] = valid && std::isfinite(plane.values.values()[i]); }
                    }
                });
                context.profile().record_computed(target_shape.elements());
                inputs = std::move(channels);
            } else inputs.resize(3);
        } else {
            const auto zenith = mode == "AGHI_ALD" ? 4u : 3u;
            const auto reflections = mode == "RF" ? 2u : 3u;
            for (std::size_t i = 0; i < reflections; ++i) solar_correct(inputs[i],inputs[zenith],distance,context);
            if (mode == "RF" || mode == "AGHI_ALD") {
                const auto infrared = mode == "RF" ? 2u : 3u;
                to_celsius(inputs[infrared]);
                const double high = percentile(inputs[infrared].values.values(),95,context);
                double ratio = 0, count = 0;
                if (mode == "RF") {
                    for (auto i = std::size_t{0}; i < shape.elements(); ++i)
                        if (!std::isnan(inputs[0].values.values()[i]) && inputs[0].values.values()[i] > 1e-2 && !std::isnan(inputs[2].values.values()[i])) {
                            ratio += (high - inputs[2].values.values()[i]) / inputs[0].values.values()[i]; ++count;
                        }
                    ratio /= count;
                    if (!std::isfinite(ratio) || ratio == 0) throw std::invalid_argument("FY4 RF background ratio is undefined");
                    output.display.mapper = [high,ratio](std::span<const float> values,const DisplayScale&) {
                        const float r = std::clamp(values[0],0.0f,3.0f), sir = std::clamp(values[1],0.0f,3.0f);
                        const float b = float((high - values[2]) / ratio), g = float(std::pow(sir,1.5)), red = float(std::pow(r,1.2));
                        return rgb(red,g,b);
                    };
                    inputs.resize(3);
                } else {
                    context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                        for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                            const bool night = inputs[4].values.values()[i] > 90;
                            float b = inputs[2].values.values()[i], g = float(std::pow(inputs[1].values.values()[i],1.3)), r = float(std::pow(inputs[0].values.values()[i],2));
                            if (night) { b = float((high - inputs[3].values.values()[i]) / 45); g = r = float((high - inputs[3].values.values()[i]) / 115); }
                            const bool valid = inputs[0].valid.values()[i] && inputs[3].valid.values()[i] && inputs[4].valid.values()[i] &&
                                (night || (inputs[1].valid.values()[i] && inputs[2].valid.values()[i])) && std::isfinite(r) && std::isfinite(g) && std::isfinite(b);
                            inputs[0].values.values()[i] = r; inputs[1].values.values()[i] = g; inputs[2].values.values()[i] = b;
                            for (std::size_t channel = 0; channel < 3; ++channel) inputs[channel].valid.values()[i] = valid;
                        }
                    });
                    context.profile().record_computed(shape.elements());
                    inputs.resize(3);
                }
            } else {
                output.display.mapper = [mode](std::span<const float> values,const DisplayScale&) {
                    if (mode == "RTpp") return rgb(std::clamp(values[0],0.0f,3.0f),std::clamp(values[1],0.0f,3.0f),std::clamp(values[2],0.0f,3.0f));
                    const float b = std::clamp(values[0],0.0f,3.0f);
                    const float g = std::clamp(mode == "RX" ? values[1] * 0.5f + values[0] * 0.5f : values[1],0.0f,3.0f);
                    const float r = std::clamp(mode == "RX" ? values[2] * 0.5f + values[0] * 0.5f : values[2],0.0f,3.0f);
                    return rgb(r,g,b);
                };
                inputs.resize(3);
            }
        }
        if (!output.display.mapper) output.display.mapper = [](std::span<const float> values,const DisplayScale&) { return rgb(values[0],values[1],values[2]); };
        for (std::size_t i = 0; i < inputs.size(); ++i) inputs[i].unit = mode == "E" || (mode == "RF" && i == 2) ? "degC" : "1";
        output.frame.planes = std::move(inputs);
        output.frame.validate();
        return output;
    };
    return result;
}

}
