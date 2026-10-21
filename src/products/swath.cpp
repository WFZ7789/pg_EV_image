/// SPDX-License-Identifier: MIT

#include "products/swath.hpp"
#include "products/statistics.hpp"
#include "products/rayleigh.hpp"
#include "common.hpp"
#include "products/palette.hpp"
#include "products/phys.hpp"
#include "sensors/hdf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace fy::products {
using namespace core;
using namespace detail;

namespace {

/// @brief Oklab 感知均匀颜色，用于色带节点的平滑插值。
struct OklabColor { float lightness; float a; float b; };

/// @brief 线性光颜色，通道范围 0..1。
struct LinearColor { float red; float green; float blue; };

/// @brief sRGB 分量转线性光。
float decode_component(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f,2.4f);
}

/// @brief 线性光分量转 sRGB。
float encode_component(float value) {
    return value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value,1.0f / 2.4f) - 0.055f;
}

/// @brief 8 位 sRGB 整数转线性光。
LinearColor to_linear(unsigned rgb) {
    return {decode_component(float((rgb >> 16) & 0xFFu) / 255.0f),
            decode_component(float((rgb >> 8) & 0xFFu) / 255.0f),
            decode_component(float(rgb & 0xFFu) / 255.0f)};
}

/// @brief 线性光转 Oklab。
OklabColor to_oklab(LinearColor color) {
    const float l = 0.4122214708f * color.red + 0.5363325363f * color.green + 0.0514459929f * color.blue;
    const float m = 0.2119034982f * color.red + 0.6806995451f * color.green + 0.1073969566f * color.blue;
    const float s = 0.0883024619f * color.red + 0.2817188376f * color.green + 0.6299787005f * color.blue;
    const float l_ = std::cbrt(l), m_ = std::cbrt(m), s_ = std::cbrt(s);
    return {0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
            1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
            0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_};
}

/// @brief Oklab 转线性光。
LinearColor to_linear(OklabColor color) {
    const float l_ = color.lightness + 0.3963377774f * color.a + 0.2158037573f * color.b;
    const float m_ = color.lightness - 0.1055613458f * color.a - 0.0638541728f * color.b;
    const float s_ = color.lightness - 0.0894841775f * color.a - 1.2914855480f * color.b;
    const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    return {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
            -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
            -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
}

/// @brief 线性光相对亮度。
float luminance(LinearColor color) {
    return 0.2126f * color.red + 0.7152f * color.green + 0.0722f * color.blue;
}

/// @brief 平滑阶跃，用于掩膜淡入淡出。
float smooth_step(float low,float high,float value) {
    const float t = std::clamp((value - low) / (high - low),0.0f,1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// @brief 等亮度混色，混入前后相对亮度不变。
/// @note 亮度只由亮温决定是夜景色的前提，因此 tint 只旋转色相。
LinearColor mix_equal_luminance(LinearColor color,float brightness,LinearColor tint,float tint_luminance,float weight) {
    if (tint_luminance <= 0) return color;
    const float scale = brightness / tint_luminance;
    return {std::lerp(color.red,tint.red * scale,weight),
            std::lerp(color.green,tint.green * scale,weight),
            std::lerp(color.blue,tint.blue * scale,weight)};
}


/// @brief 用邻域有效均值补齐单像元级缺测，仅用于显示。
/// @param plane 待补的数值/掩膜平面。
/// @param context 执行上下文，用于行并行。
/// @param radius 邻域半径（1 或 2）。
/// @param passes 迭代次数，逐步封合较宽的缝隙。
void fill_small_gaps(ScalarPlane& plane,ExecutionContext& context,int radius,int passes) {
    const auto shape = plane.values.shape();
    auto values = plane.values.values();
    auto valid = plane.valid.values();
    std::vector<float> patch(shape.elements(), NAN);
    std::vector<std::uint8_t> filled(shape.elements(), 0);
    for (int pass = 0; pass < passes; ++pass) {
        std::fill(patch.begin(),patch.end(),NAN);
        std::fill(filled.begin(),filled.end(),0);
        context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
            for (std::size_t row = begin; row < end; ++row)
                for (std::size_t col = 0; col < shape.cols; ++col) {
                    const auto index = row * shape.cols + col;
                    if (valid[index]) continue;
                    float sum = 0;
                    int count = 0;
                    for (int dy = -radius; dy <= radius; ++dy)
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const auto r = static_cast<std::int64_t>(row) + dy, c = static_cast<std::int64_t>(col) + dx;
                            if (r < 0 || c < 0 || r >= static_cast<std::int64_t>(shape.rows) || c >= static_cast<std::int64_t>(shape.cols)) continue;
                            const auto neighbor = static_cast<std::size_t>(r) * shape.cols + static_cast<std::size_t>(c);
                            if (!valid[neighbor] || !std::isfinite(values[neighbor])) continue;
                            sum += values[neighbor]; ++count;
                        }
                    if (count) { patch[index] = sum / float(count); filled[index] = 1; }
                }
        });
        for (std::size_t i = 0; i < shape.elements(); ++i)
            if (filled[i]) { values[i] = patch[i]; valid[i] = 1; }
    }
}

/// @brief 月光夜景 9 节点色带，温度单位为 10.8 μm 亮温 (°C)。
constexpr std::array<std::pair<float,unsigned>,9> kMoonStops{{
    {20,0x050912},{0,0x0b1630},{-20,0x1c2f5c},{-35,0x34508a},{-50,0x5f80b8},
    {-62,0x93acd4},{-72,0xc4d2e8},{-82,0xeaf0f8},{-90,0xfff6e6}}};

/// @brief 把色带节点在 Oklab 中插值成 256 级线性光查找表。
const std::array<LinearColor,256>& moon_palette() {
    static const auto table = [] {
        std::array<OklabColor,9> nodes{};
        for (std::size_t i = 0; i < nodes.size(); ++i) nodes[i] = to_oklab(to_linear(kMoonStops[i].second));
        std::array<LinearColor,256> result{};
        for (std::size_t i = 0; i < result.size(); ++i) {
            const float temperature = 20.0f - 110.0f * float(i) / 255.0f;
            std::size_t segment = 0;
            while (segment + 2 < kMoonStops.size() && temperature < kMoonStops[segment + 1].first) ++segment;
            const float high = kMoonStops[segment].first, low = kMoonStops[segment + 1].first;
            const float fraction = std::clamp((high - temperature) / (high - low),0.0f,1.0f);
            result[i] = to_linear(OklabColor{std::lerp(nodes[segment].lightness,nodes[segment + 1].lightness,fraction),
                                             std::lerp(nodes[segment].a,nodes[segment + 1].a,fraction),
                                             std::lerp(nodes[segment].b,nodes[segment + 1].b,fraction)});
        }
        return result;
    }();
    return table;
}

}

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
    const bool composite_mode = mode == "EC" || mode == "EK" || mode == "ECT";
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
        if (!request.color.empty() && request.color != "linear" && request.color != "srgb" && request.color != "contrast")
            throw std::invalid_argument("RT curve must be linear, srgb or contrast");
        ids = {1,2,3,sensors::solar_zenith,sensors::solar_azimuth,sensors::satellite_zenith,sensors::satellite_azimuth};
        units = {"1","1","1"};
    } else if (mode == "RY" || mode == "RF") {
        allowed(satellite,{"FY-3D","FY-3F","FY-3H","FY-3G"});
        const int red = satellite == "FY-3G" ? 1 : 3;
        ids = {red,red + 1,mode == "RY" ? (satellite == "FY-3G" ? 5 : 6) : (satellite == "FY-3G" ? 7 : 24),sensors::solar_zenith};
        units = {"1","1",mode == "RY" ? "1" : "degC"};
    } else if (composite_mode) {
        allowed(satellite,mode == "EC" || mode == "ECT" ? std::initializer_list<std::string_view>{"FY-3D","FY-3F","FY-3H","FY-3E","FY-3G"} :
                                           std::initializer_list<std::string_view>{"FY-3D","FY-3F","FY-3H","FY-3E"});
        if (quarter) throw std::invalid_argument("EC/EK require the native non-250M primary file");
        const int middle = satellite == "FY-3E" ? 2 : satellite == "FY-3G" ? 6 : 20;
        const int infrared = satellite == "FY-3E" ? 6 : satellite == "FY-3G" ? 7 : 24;
        if (mode == "EC" || mode == "ECT") {
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
    const int shadow_distance = mode == "EC" && auxiliary ? 4 : 1;
    for (int id : ids) {
        band_at(bands,id);
        plan.inputs.push_back({id,plan.calculation_grid,composite_mode ? 10u * shadow_distance : 0u,
                               composite_mode ? 10u * shadow_distance : 0u,false,composite_mode || mode == "RF"});
        plan.inputs.back().cyclic_degrees = mode == "RT" && (id == sensors::solar_azimuth || id == sensors::satellite_azimuth);
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
    if (mode == "RT") {
        auto& spec = statistic(StatisticsOperation::minmax,"reflection_rgb");
        spec.include_masked_finite = false; spec.exclude_nan_only = false; spec.upper_cutoff = 2.5;
        spec.domain = request.mosaic ? StatisticsDomain::task : StatisticsDomain::scene;
    }
    if (mode == "RY" || mode == "RMUL") {
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
    if (composite_mode) {
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
    result.prepare = [metadata,request,description,count,infrared_index,second_index,zenith_index,azimuth_index,shadow_distance](std::vector<ScalarPlane>&& inputs,
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
            const auto solar_index = mode == "RT" ? 3 : inputs.size() - 1;
            const auto reflection_count = mode == "RF" ? 2 : solar_index;
            for (std::size_t i = 0; i < reflection_count; ++i)
                solar_correct(inputs[i],inputs[solar_index],metadata.numeric_attributes.at("earth_sun_distance_au"),context);
            if (mode == "RT")
                correct_rayleigh(std::span(inputs).first(3),inputs[3],inputs[4],inputs[5],inputs[6],context);
            inputs.resize(solar_index);
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
            } else if (mode == "RT") {
                double low = INFINITY,high = -INFINITY;
                for (auto& plane : inputs)
                    for (std::size_t i = 0; i < shape.elements(); ++i) {
                        const float value = plane.values.values()[i];
                        if (!plane.valid.values()[i] || !std::isfinite(value) || value > 2.5f) {
                            plane.valid.values()[i] = 0; plane.values.values()[i] = NAN;
                        } else {
                            low = std::min(low,double(value)); high = std::max(high,double(value));
                        }
                    }
                if (!std::isfinite(low)) throw std::invalid_argument("RT has no valid reflection samples at or below 2.5");
                output.display.scale = {low,high == low ? low + 1 : high};
                output.display.balanced_rgb = true;
                output.display.mapper = [curve = request.color.empty() ? std::string("contrast") : request.color](std::span<const float> values,const DisplayScale& scale) {
                    const auto normalize = [&](float value,std::size_t channel) {
                        float result = float(std::clamp((value * scale.rgb_gain[channel] - scale.low) / (scale.high - scale.low),0.0,1.0));
                        if (curve == "contrast") result = result * result * (3 - 2 * result);
                        return curve == "linear" ? result : encode_component(result);
                    };
                    return rgb(normalize(values[2],0),normalize(values[1],1),normalize(values[0],2));
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
            auto shade = shadow(inputs[infrared_index],inputs[zenith_index],inputs[azimuth_index],context,shadow_distance);
            if (mode == "ECT") {
                /// @note 亮度只由亮温决定；雾与卷云只旋转色相，等亮度混合避免温度误读。
                const auto& palette = moon_palette();
                const auto fog = to_linear(0x2fbfa8u), cirrus_tint = to_linear(0xa78bdcu);
                const float fog_luminance = luminance(fog), cirrus_luminance = luminance(cirrus_tint);
                const bool cirrus_supported = metadata.satellite == "FY-3D" || metadata.satellite == "FY-3E";
                /// @note 3.8-10.8 的绝对量级随定标与场景漂移，雾门限取夜间像元中位数加偏置。
                constexpr float histogram_low = -30.0f, histogram_step = 0.2f;
                constexpr std::size_t histogram_bins = 400;
                std::array<std::size_t,histogram_bins> histogram{};
                std::size_t night_samples = 0;
                for (std::size_t i = 0; i < shape.elements(); ++i) {
                    const float infrared = inputs[infrared_index].values.values()[i];
                    const float zenith = inputs[zenith_index].values.values()[i];
                    if (!std::isfinite(infrared) || !std::isfinite(zenith) || zenith <= 90 || infrared <= -40) continue;
                    const float difference = inputs[0].values.values()[i] - infrared;
                    if (!std::isfinite(difference)) continue;
                    const auto bin = static_cast<std::size_t>(std::clamp((difference - histogram_low) / histogram_step,0.0f,float(histogram_bins - 1)));
                    ++histogram[bin]; ++night_samples;
                }
                float baseline = 0;
                if (night_samples) {
                    std::size_t accumulated = 0;
                    for (std::size_t bin = 0; bin < histogram_bins; ++bin) {
                        accumulated += histogram[bin];
                        if (accumulated * 2 >= night_samples) { baseline = histogram_low + (float(bin) + 0.5f) * histogram_step; break; }
                    }
                }
                /// @note 3.8 μm 通道存在单像元级缺测，先补洞再合成，避免画面上出现黑色斜纹。
                fill_small_gaps(inputs[0],context,2,2);
                context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                    for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                        const float middle = inputs[0].values.values()[i], reference = inputs[1].values.values()[i];
                        const float infrared = inputs[infrared_index].values.values()[i];
                        const float zenith = inputs[zenith_index].values.values()[i];
                        const bool valid = shade.valid.values()[i] && inputs[0].valid.values()[i] && inputs[1].valid.values()[i] &&
                                           inputs[infrared_index].valid.values()[i] && inputs[second_index].valid.values()[i] &&
                                           std::isfinite(infrared);
                        float red = NAN, green = NAN, blue = NAN;
                        if (valid) {
                            const float offset = std::clamp((20.0f - infrared) * (255.0f / 110.0f),0.0f,255.0f);
                            const auto index = std::min<std::size_t>(254,static_cast<std::size_t>(offset));
                            const float fraction = offset - static_cast<float>(index);
                            const auto low_color = palette[index], high_color = palette[index + 1];
                            auto color = LinearColor{std::lerp(low_color.red,high_color.red,fraction),
                                                     std::lerp(low_color.green,high_color.green,fraction),
                                                     std::lerp(low_color.blue,high_color.blue,fraction)};
                            const float brightness = luminance(color);
                            const float fog_weight = 0.65f * smooth_step(baseline + 3.0f,baseline + 6.0f,middle - infrared) *
                                                     smooth_step(88.0f,92.0f,zenith) * smooth_step(-45.0f,-30.0f,infrared);
                            if (fog_weight > 0)
                                color = mix_equal_luminance(color,brightness,fog,fog_luminance,fog_weight);
                            if (cirrus_supported) {
                                const float cirrus_weight = 0.55f * smooth_step(3.0f,6.0f,reference - infrared) *
                                                            smooth_step(-65.0f,-55.0f,infrared) *
                                                            (1.0f - smooth_step(-15.0f,-5.0f,infrared));
                                if (cirrus_weight > 0)
                                    color = mix_equal_luminance(color,brightness,cirrus_tint,cirrus_luminance,cirrus_weight);
                            }
                            red = encode_component(std::clamp(color.red,0.0f,1.0f));
                            green = encode_component(std::clamp(color.green,0.0f,1.0f));
                            blue = encode_component(std::clamp(color.blue,0.0f,1.0f));
                        }
                        inputs[0].values.values()[i] = blue;
                        inputs[1].values.values()[i] = green;
                        inputs[2].values.values()[i] = red;
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            inputs[channel].valid.values()[i] = valid ? 1 : 0;
                    }
                });
                inputs.resize(3);
                for (auto& plane : inputs) plane.unit = "1";
                output.display.mapper = [](std::span<const float> values,const DisplayScale&) {
                    return rgb(values[2],values[1],values[0]);
                };
            } else if (mode == "EC") {
                ScalarPlane maximum{"1",Raster<float>(shape),Raster<std::uint8_t>(shape)};
                context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
                    for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
                        const float middle = inputs[0].values.values()[i], reference = inputs[1].values.values()[i];
                        const float infrared = inputs[infrared_index].values.values()[i], second = inputs[second_index].values.values()[i];
                        /// @note 冷云顶将 3.8 μm 权重降至零，同时跳过缺测值，避免 0 * NaN 和无效掩膜。
                        const float coefficient = smooth_step(-60.0f,-40.0f,infrared) *
                                                  float(0.5 * std::tanh(infrared * 0.1 + 5) + 0.5);
                        const bool middle_valid = coefficient == 0 ||
                                                  (inputs[0].valid.values()[i] && std::isfinite(middle));
                        const float blended = coefficient == 0 ? second : coefficient * middle + (1 - coefficient) * second;
                        float red = (25 - reference) / 115, green = (25 - infrared) / 115;
                        float blue = (25 - blended) / 115;
                        blue = float(0.36 * std::cos(9 * blue - 1.5) + 0.6 * blue + 0.12);
                        const float gain = -shade.values.values()[i];
                        const float shadow = float(std::pow((red + green + blue) / 3,3)) * gain;
                        red += shadow; green += shadow; blue += shadow;
                        const bool valid = shade.valid.values()[i] && middle_valid && inputs[1].valid.values()[i] &&
                                           inputs[infrared_index].valid.values()[i] && inputs[second_index].valid.values()[i] &&
                                           std::isfinite(red) && std::isfinite(green) && std::isfinite(blue);
                        inputs[0].values.values()[i] = blue;
                        inputs[1].values.values()[i] = green;
                        inputs[2].values.values()[i] = red;
                        for (std::size_t channel = 0; channel < 3; ++channel) inputs[channel].valid.values()[i] = valid;
                        maximum.values.values()[i] = valid ? std::max({red,green,blue}) : NAN;
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
        if (mode == "RT" && !request.mosaic) {
            SharedDisplayStatistics statistics("RT",context.profile());
            statistics.add(output); output.display.scale = *statistics.scale();
        }
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
