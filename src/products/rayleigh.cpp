/// SPDX-License-Identifier: MIT

#include "products/rayleigh.hpp"
#include "products/phys.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fy::products {
namespace {

/// @note Vermote & Tanré (1992), doi:10.1016/0022-4073(92)90149-X。
struct MolecularGeometry {
    double solar,view,azimuth,second_azimuth;
    std::array<double,3> phase;
    double fit_constant,fit_log;

    MolecularGeometry(double mus,double muv,double phi) : solar(mus),view(muv),azimuth(std::cos(phi * Deg2rad)),
        second_azimuth(2 * azimuth * azimuth - 1) {
        constexpr double depolarization = 0.0279;
        constexpr double factor = 2 * (1 - depolarization) / (2 + depolarization);
        const double s2 = mus * mus,v2 = muv * muv;
        phase = {1 + factor * (3 * s2 - 1) * (3 * v2 - 1) / 8,
                 -0.75 * factor * mus * muv * std::sqrt((1 - s2) * (1 - v2)),
                 0.1875 * factor * (1 - s2) * (1 - v2)};
        const std::array<double,5> terms{1,mus + muv,mus * muv,s2 + v2,s2 * v2};
        constexpr std::array<double,5> a{0.33243832,0.16285370,-0.30924818,-0.10324388,0.11493334};
        constexpr std::array<double,5> b{-0.06777104,0.001577425,-0.01240906,0.03241678,-0.03503695};
        fit_constant = fit_log = 0;
        for (std::size_t i = 0; i < terms.size(); ++i) {
            fit_constant += a[i] * terms[i];
            fit_log += b[i] * terms[i];
        }
    }

    double reflectance(double tau,double log_tau) const {
        const double single = -std::expm1(-tau * (1 / solar + 1 / view)) / (4 * (solar + view));
        const double multiple = std::expm1(-tau / solar) * std::expm1(-tau / view);
        const double zero = phase[0] * (single + multiple * (fit_constant + log_tau * fit_log));
        const double first = phase[1] * (single + multiple * (0.19666292 - 0.05439061 * log_tau));
        const double second = phase[2] * (single + multiple * (0.14545937 - 0.02910845 * log_tau));
        return std::max(0.0,zero - 2 * first * azimuth + 2 * second * second_azimuth);
    }
};

}

double rayleigh_optical_depth(double wavelength_um) {
    if (!std::isfinite(wavelength_um) || wavelength_um <= 0) throw std::invalid_argument("Invalid Rayleigh wavelength");
    const double inverse2 = 1 / (wavelength_um * wavelength_um),inverse4 = inverse2 * inverse2;
    return 0.008569 * inverse4 * (1 + 0.0113 * inverse2 + 0.00013 * inverse4);
}

double rayleigh_path_reflectance(double tau,double mus,double muv,double azimuth) {
    if (!std::isfinite(tau) || tau < 0 || !std::isfinite(mus) || mus <= 0 || mus > 1 ||
        !std::isfinite(muv) || muv <= 0 || muv > 1 || !std::isfinite(azimuth)) return NAN;
    if (tau == 0) return 0;
    return MolecularGeometry(mus,muv,azimuth).reflectance(tau,std::log(tau));
}

void correct_rayleigh(std::span<core::ScalarPlane> bgr,const core::ScalarPlane& sunz,
                      const core::ScalarPlane& suna,const core::ScalarPlane& satz,
                      const core::ScalarPlane& sata,core::ExecutionContext& context) {
    if (bgr.size() != 3) throw std::invalid_argument("Rayleigh correction needs B/G/R planes");
    const auto shape = bgr[0].values.shape();
    for (const auto& plane : bgr)
        if (plane.values.shape() != shape || plane.valid.shape() != shape)
            throw std::invalid_argument("Rayleigh reflectance shapes differ");
    const std::array<const core::ScalarPlane*,4> angles{&sunz,&suna,&satz,&sata};
    for (const auto* angle : angles)
        if (angle->values.shape() != shape || angle->valid.shape() != shape || angle->unit != "degree")
            throw std::invalid_argument("Rayleigh geometry must contain matching degree planes");
    auto phase = context.profile().phase("rayleigh");
    const std::array<double,3> tau{rayleigh_optical_depth(0.470),rayleigh_optical_depth(0.550),rayleigh_optical_depth(0.650)};
    const std::array<double,3> log_tau{std::log(tau[0]),std::log(tau[1]),std::log(tau[2])};
    context.parallel_rows(shape.rows,[&](std::size_t begin,std::size_t end) {
        for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
            bool valid = true;
            for (const auto* angle : angles)
                valid = valid && angle->valid.values()[i] && std::isfinite(angle->values.values()[i]);
            const double solar = sunz.values.values()[i],view = satz.values.values()[i];
            const float red = bgr[2].values.values()[i];
            valid = valid && solar >= 0 && solar < 90 && view >= 0 && view < 90 &&
                    bgr[2].valid.values()[i] && std::isfinite(red) && red <= 2.5f;
            if (!valid) {
                for (auto& plane : bgr) { plane.values.values()[i] = NAN; plane.valid.values()[i] = 0; }
                continue;
            }
            const double cloud = std::clamp((1.0 - red) / 0.8,0.0,1.0);
            const double strength = cloud * std::clamp((90 - std::max(solar,view)) / 20,0.0,1.0);
            const MolecularGeometry geometry(std::cos(std::min(solar,80.0) * Deg2rad),std::cos(std::min(view,80.0) * Deg2rad),
                                             double(suna.values.values()[i]) - sata.values.values()[i]);
            for (std::size_t channel = 0; channel < bgr.size(); ++channel) {
                auto& plane = bgr[channel];
                const float value = plane.values.values()[i];
                const bool good = plane.valid.values()[i] && std::isfinite(value) && value <= 2.5f;
                plane.values.values()[i] = good ? float(std::max(0.0,value - strength * geometry.reflectance(tau[channel],log_tau[channel]))) : NAN;
                plane.valid.values()[i] = good;
            }
        }
    });
    context.profile().record_computed(shape.elements());
}

}
