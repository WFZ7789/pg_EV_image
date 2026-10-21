/// SPDX-License-Identifier: MIT

#include "products/rayleigh.hpp"
#include "products/phys.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <source_location>

namespace {

void require(bool value,std::source_location location = std::source_location::current()) {
    if (!value) throw std::runtime_error("Rayleigh check failed at line " + std::to_string(location.line()));
}

}

int main() {
    using namespace fy;
    constexpr std::array<double,3> wavelengths{0.47,0.55,0.65};
    /// @note 独立 Satpy _chand 双精度参考值；只用于检验论文公式和方位角约定。
    constexpr std::array<std::array<double,6>,6> reference{{
        {0,0,0,0.07143421509656213,0.03736290223989929,0.01873946063962237},
        {30,0,0,0.07204638290559921,0.037776211251123674,0.01897377346254877},
        {30,45,0,0.10899701274288578,0.05795194841065237,0.029330239127498957},
        {30,45,180,0.06359741690881082,0.03332042790643223,0.01671625399291709},
        {60,65,90,0.16696933166180083,0.09138809195671628,0.04682217470769112},
        {80,75,135,0.6751015746784879,0.4421671582152366,0.2540419847071705}}};
    for (const auto& sample : reference)
        for (std::size_t band = 0; band < 3; ++band) {
            const auto tau = products::rayleigh_optical_depth(wavelengths[band]);
            const double mus = std::cos(sample[0] * Deg2rad),muv = std::cos(sample[1] * Deg2rad);
            const auto actual = products::rayleigh_path_reflectance(tau,mus,muv,sample[2]);
            require(std::abs(actual - sample[3 + band]) < 1e-9);
            require(std::abs(actual - products::rayleigh_path_reflectance(tau,muv,mus,sample[2])) < 1e-14);
            require(std::abs(actual - products::rayleigh_path_reflectance(tau,mus,muv,sample[2] + 360)) < 1e-14);
        }
    require(products::rayleigh_path_reflectance(0,1,1,0) == 0);
    require(std::isnan(products::rayleigh_path_reflectance(0.1,-0.1,1,0)));
    require(std::isnan(products::rayleigh_path_reflectance(0.1,1,1,NAN)));
    const double depolarized_phase = 1 + (2 * (1 - 0.0279) / (2 + 0.0279)) / 2;
    require(std::abs(products::rayleigh_path_reflectance(1e-9,1,1,0) / 1e-9 - depolarized_phase / 4) < 1e-8);
    const core::Shape shape{32,16};
    auto plane = [&](const char* unit,float value) {
        core::ScalarPlane result{unit,core::Raster<float>(shape),core::Raster<std::uint8_t>(shape)};
        std::fill(result.values.values().begin(),result.values.values().end(),value);
        std::fill(result.valid.values().begin(),result.valid.values().end(),1);
        return result;
    };
    auto sunz = plane("degree",30),satz = plane("degree",0),suna = plane("degree",359),sata = plane("degree",1);
    auto inputs = [&] {
        std::vector<core::ScalarPlane> result;
        for (int band = 0; band < 3; ++band) result.push_back(plane("1",0.1));
        for (auto& band : result) {
            band.values.values()[1] = 1;
            band.values.values()[2] = 0.6;
        }
        result[0].values.values()[3] = 2.5001f;
        return result;
    };
    sunz.valid.values()[4] = 0;
    satz.values.values()[5] = NAN;
    sunz.values.values()[6] = 90;
    sunz.values.values()[7] = 80;
    satz.values.values()[8] = 90;
    std::vector<core::ScalarPlane> previous;
    for (int threads : {1,4}) {
        core::ExecutionContext context({.threads = threads});
        auto data = inputs();
        products::correct_rayleigh(data,sunz,suna,satz,sata,context);
        for (std::size_t band = 0; band < 3; ++band) {
            const auto& output = data[band];
            require(std::abs(output.values.values()[0] - (0.1 - reference[1][band + 3])) < 1e-8);
            require(output.values.values()[1] == 1);
            require(std::abs(output.values.values()[2] - (0.6 - reference[1][band + 3] / 2)) < 1e-7);
            for (const auto i : {4,5,6,8}) require(!output.valid.values()[i] && std::isnan(output.values.values()[i]));
            const auto path = products::rayleigh_path_reflectance(products::rayleigh_optical_depth(wavelengths[band]),
                                                                  std::cos(80 * Deg2rad),1,358);
            require(std::abs(output.values.values()[7] - std::max(0.0,0.1 - path / 2)) < 1e-8);
            if (!previous.empty())
                for (std::size_t i = 0; i < shape.elements(); ++i) {
                    require(output.valid.values()[i] == previous[band].valid.values()[i]);
                    require(output.values.values()[i] == previous[band].values.values()[i] ||
                            (std::isnan(output.values.values()[i]) && std::isnan(previous[band].values.values()[i])));
                }
        }
        require(!data[0].valid.values()[3] && std::isnan(data[0].values.values()[3]));
        previous = std::move(data);
    }
}
