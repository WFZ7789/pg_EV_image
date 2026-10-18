/// SPDX-License-Identifier: MIT

#include "products/fy4.hpp"
#include "common.hpp"

namespace fy::products {

using namespace core;
using namespace detail;

void correct_agri_day(ScalarPlane& red_plane,ScalarPlane& nir_plane,ScalarPlane& blue_plane,
                      const ScalarPlane& solar_zenith,const ScalarPlane& satellite_zenith,
                      const std::string& satellite,double distance,ExecutionContext& context) {
    const auto shape = red_plane.values.shape();
    if (!shape.elements() || red_plane.valid.shape() != shape || !std::isfinite(distance) || distance <= 0)
        throw std::invalid_argument("Invalid AGRI day correction inputs");
    allowed(satellite,{"FY-4A","FY-4B"});
    for (const auto* plane : std::array<const ScalarPlane*,4>{&nir_plane,&blue_plane,&solar_zenith,&satellite_zenith})
        if (plane->values.shape() != shape || plane->valid.shape() != shape)
            throw std::invalid_argument("AGRI day correction grid mismatch");
    const float distance_scale = float(distance * distance);
    context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
        for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
            const float solar = solar_zenith.values.values()[i], view = satellite_zenith.values.values()[i];
            const float mu_s = std::max(std::cos(solar * float(Deg2rad)),0.05f);
            const float scale = distance_scale / mu_s;
            const float raw_blue = blue_plane.values.values()[i] * scale;
            const float raw_red = red_plane.values.values()[i] * scale;
            const float raw_nir = nir_plane.values.values()[i] * scale;
            if (!std::isfinite(raw_blue) || !std::isfinite(raw_red) || !std::isfinite(raw_nir) ||
                !std::isfinite(solar) || !std::isfinite(view) || solar >= 90 || view >= 90) {
                for (auto* plane : {&red_plane,&nir_plane,&blue_plane}) {
                    plane->values.values()[i] = NAN; plane->valid.values()[i] = 0;
                }
                continue;
            }
            const float mu_v = std::max(std::cos(view * float(Deg2rad)),0.05f);
            const float weight = 1.0f - std::clamp(std::max({raw_blue,raw_red,raw_nir}) / 0.6f,0.0f,1.0f);
            std::array<float,3> path;
            constexpr std::array coefficients{0.1726f,0.0501f,0.0187f};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float tau = coefficients[channel];
                path[channel] = (1.0f - std::exp(-tau / mu_s - tau / mu_v)) /
                                 (2.0f * (1.0f / mu_s + 1.0f / mu_v));
            }
            const auto blue = std::max(raw_blue - path[0] * weight,0.0f);
            const auto red = std::max(raw_red - path[1] * weight,0.0f);
            const auto nir = std::max(raw_nir - path[2] * weight,0.0f);
            const float output_red = -0.1992f * blue + 1.3768f * red - 0.1770f * nir;
            const float output_green = 0.4500f * blue + 0.4500f * red + 0.1000f * nir;
            const float output_blue = 1.0000f * blue + 0.0000f * red + 0.0000f * nir;
            float r = std::pow(std::max(output_red,0.0f),satellite == "FY-4B" ? 0.87f : 0.85f) *
                      (satellite == "FY-4B" ? 0.90f : 0.90873f);
            float g = std::pow(std::max(output_green,0.0f),0.85f);
            float b = std::pow(std::max(output_blue,0.0f),0.75f) * 1.06f;
            if (satellite == "FY-4B") {
                const float peak = std::max({r,g,b});
                const float gain = peak > 0.82f ? (0.82f + 0.16f * (1.0f - std::exp(-(peak - 0.82f) / 0.16f))) / peak : 1.0f;
                r *= gain; g *= gain; b *= gain;
            }
            const bool valid = red_plane.valid.values()[i] && nir_plane.valid.values()[i] && blue_plane.valid.values()[i] &&
                               solar_zenith.valid.values()[i] && satellite_zenith.valid.values()[i];
            red_plane.values.values()[i] = r; nir_plane.values.values()[i] = g; blue_plane.values.values()[i] = b;
            for (auto* plane : {&red_plane,&nir_plane,&blue_plane}) plane->valid.values()[i] = valid;
        }
    });
    context.profile().record_computed(shape.elements());
}

}
