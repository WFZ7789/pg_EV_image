/// SPDX-License-Identifier: MIT

#pragma once

#include "products/swath.hpp"
#include "products/palette.hpp"
#include "products/phys.hpp"
#include "scan_sampling.hpp"

#include <algorithm>
#include <cmath>

namespace fy::products::detail {

using namespace core;

inline const BandDescriptor& band_at(std::span<const BandDescriptor> bands, int id) {
    auto found = std::ranges::find(bands,id,&BandDescriptor::id);
    if (found == bands.end()) throw std::invalid_argument("Product input band is missing: " + std::to_string(id));
    return *found;
}

inline void allowed(const std::string& satellite, std::initializer_list<std::string_view> supported) {
    if (std::ranges::find(supported,satellite) == supported.end())
        throw std::invalid_argument("Unsupported product for satellite: " + satellite);
}

inline void validate_planes(const std::vector<ScalarPlane>& planes, std::size_t count, Shape shape) {
    if (planes.size() != count) throw std::invalid_argument("Product input count mismatch");
    for (const auto& plane : planes)
        if (plane.values.shape() != shape || plane.valid.shape() != shape)
            throw std::invalid_argument("Product calculation grid mismatch");
}

template<class F>
void transform(ScalarPlane& plane, ExecutionContext& context, F&& function) {
    const auto shape = plane.values.shape();
    auto values = plane.values.values();
    auto valid = plane.valid.values();
    context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
        for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
            const float value = std::isfinite(values[i]) ? function(values[i],i) : NAN;
            valid[i] = valid[i] && std::isfinite(value);
            values[i] = std::isfinite(value) ? value : NAN;
        }
    });
    context.profile().record_computed(shape.elements());
}

inline void celsius(ScalarPlane& plane, ExecutionContext& context) {
    if (plane.unit != "K") throw std::invalid_argument("Thermal input must be in kelvin");
    transform(plane,context,[](float value,std::size_t) { return static_cast<float>(value + K2c); });
    plane.unit = "degC";
}

inline void solar_correct(ScalarPlane& plane, const ScalarPlane& solar, double distance, ExecutionContext& context) {
    const bool percent = plane.unit == "%";
    if (!percent && plane.unit != "1") throw std::invalid_argument("Reflection input has an unsupported unit");
    const auto factor = static_cast<float>(std::pow(distance,2));
    transform(plane,context,[&](float value,std::size_t index) {
        plane.valid.values()[index] = plane.valid.values()[index] && solar.valid.values()[index];
        if (percent) value /= 1e2;
        value *= factor / static_cast<float>(std::cos(solar.values.values()[index] * Deg2rad));
        return value;
    });
    plane.unit = "1";
}

inline DisplayScale extrema(const ScalarPlane& plane, double low_limit, double high_limit) {
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (std::size_t i = 0; i < plane.values.values().size(); ++i)
        if (std::isfinite(plane.values.values()[i])) {
            low = std::min(low,double(plane.values.values()[i]));
            high = std::max(high,double(plane.values.values()[i]));
        }
    if (!std::isfinite(low)) throw std::invalid_argument("Product has no valid numeric samples");
    low = std::max(low,low_limit); high = std::min(high,high_limit);
    if (high == low) high = low + 1;
    return {low,high};
}

inline DisplayDescriptor scalar_display(const std::string& scheme, palette::Color8 (*fallback)(double,const RenderConfig&),
                                 DisplayScale scale, bool colorbar = true) {
    auto mapper = palette::pick_color(scheme,fallback);
    DisplayDescriptor display;
    display.scale = scale; display.colorbar = colorbar;
    display.mapper = [mapper = std::move(mapper)](std::span<const float> values,const DisplayScale& scale) {
        const auto color = mapper(values[0],{float(scale.low),float(scale.high)});
        return Color{color[2]/255.0,color[1]/255.0,color[0]/255.0};
    };
    return display;
}

inline std::vector<std::string> labels(const SensorMetadata& metadata, const std::string& description, bool mersi) {
    auto result = metadata.labels;
    result.resize(5);
    result[4] = description;
    if (mersi) {
        auto separator = metadata.observation_begin.find('T');
        if (separator != std::string::npos) {
            auto time = metadata.observation_begin.substr(separator + 1);
            auto fraction = time.find('.');
            if (fraction != std::string::npos) time.resize(fraction);
            result[2] = time + "(UTC)";
        }
    }
    return result;
}

inline ScalarPlane shadow(const ScalarPlane& temperature, const ScalarPlane& zenith, const ScalarPlane& azimuth,
                    ExecutionContext& context, int pixel_distance = 1,std::size_t scan_rows = 0,std::size_t first_row = 0,
                    const SwathGeometry* geometry = nullptr) {
    const auto shape = temperature.values.shape();
    std::optional<ScanTemperatureSampler> sampler;
    if (geometry && geometry->scan_rows) sampler.emplace(temperature,*geometry);
    auto scan_memory = context.profile().reserve(BufferKind::statistics,sampler ? sampler->bytes() : 0);
    double average_zenith = 0, average_azimuth = 0, count = 0;
    for (std::size_t i = 0; i < shape.elements(); ++i)
        if (zenith.valid.values()[i] && azimuth.valid.values()[i] && zenith.values.values()[i] > 0 &&
            zenith.values.values()[i] < 180) {
            const float z = zenith.values.values()[i], a = azimuth.values.values()[i];
            average_zenith += z > 90 ? 180 - z : z;
            average_azimuth += a > 180 ? 360 - a : a;
            ++count;
        }
    ScalarPlane result{"1",Raster<float>(shape),Raster<std::uint8_t>(shape)};
    const float elevation = count ? static_cast<float>(90 - average_zenith / count) : 90;
    const float angle = count ? static_cast<float>(average_azimuth / count * Deg2rad) : 0;
    const double raw_offset = 15.0f / std::tan(elevation * Deg2rad);
    const int offset = !std::isfinite(raw_offset) || raw_offset >= 10 ? 10 : raw_offset <= 1 ? 1 : static_cast<int>(raw_offset);
    context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
        for (auto row = begin; row < end; ++row)
            for (std::size_t col = 0; col < shape.cols; ++col) {
                const auto index = row * shape.cols + col;
                float sum = 0;
                bool valid = temperature.valid.values()[index];
                for (int scale = 1; scale <= offset && count; ++scale) {
                    const int dx = static_cast<int>(std::cos(angle) * scale * pixel_distance);
                    const int dy = static_cast<int>(std::sin(angle) * scale * pixel_distance);
                    const auto r = static_cast<std::int64_t>(row) + dy, c = static_cast<std::int64_t>(col) + dx;
                    float shifted = scan_rows ? temperature.values.values()[index] : 0;
                    if (sampler) {
                        const auto sample = sampler->offset(row,col,dy,dx);
                        if (!sample) continue;
                        shifted = *sample;
                    } else if (r >= 0 && c >= 0 && r < static_cast<std::int64_t>(shape.rows) && c < static_cast<std::int64_t>(shape.cols)) {
                        const auto neighbor = static_cast<std::size_t>(r) * shape.cols + static_cast<std::size_t>(c);
                        if (scan_rows && ((first_row+row)/scan_rows != (first_row+std::size_t(r))/scan_rows ||
                            !temperature.valid.values()[neighbor])) continue;
                        valid = valid && temperature.valid.values()[neighbor];
                        shifted = temperature.values.values()[neighbor];
                    }
                    const float difference = temperature.values.values()[index] - shifted;
                    const float term = std::min(difference,0.0f) * (1.0f / scale) * 0.1f;
                    sum += term;
                }
                result.valid.values()[index] = valid && std::isfinite(sum);
                result.values.values()[index] = std::isfinite(sum) ? std::min(sum,0.3f) : NAN;
            }
    });
    context.profile().record_computed(shape.elements());
    return result;
}

inline Color rgb(float red, float green, float blue, float divisor = 1) {
    return {red / double(divisor),green / double(divisor),blue / double(divisor)};
}


}
