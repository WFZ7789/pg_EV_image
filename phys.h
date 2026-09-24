#ifndef PG_EV_IMAGE_PHYS_H
#define PG_EV_IMAGE_PHYS_H

#include "stl.h"

inline constexpr double Hc_k = 1.4387768775039337;
inline constexpr double H2c_2 = 1.1910429723971884140794892e-5;
inline constexpr double Deg2rad = std::numbers::pi / 180.0;
inline constexpr double K2c = -273.15;

/// @brief 普朗克逆变换，由辐亮度与等效中心波数计算等效黑体亮温
/// @param rad 辐亮度 mW/(m2.cm-1.sr)
/// @param wvn 等效中心波数 cm-1
/// @return 等效黑体亮温 K
[[nodiscard]] inline double brtemp(double rad, double wvn) noexcept {
    const double wvn3 = wvn * wvn * wvn;
    return Hc_k * wvn / std::log(H2c_2 * wvn3 / rad + 1.0);
}

#endif
