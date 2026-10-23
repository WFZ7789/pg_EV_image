/// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <string_view>

namespace fy::sensors {

inline constexpr std::array<std::string_view,20> metimage_channels{
    "443","555","668","752","763","865","914","1240","1375","1630","2250",
    "3740","3959","4050","6725","7325","8540","10690","12020","13345"};

}
