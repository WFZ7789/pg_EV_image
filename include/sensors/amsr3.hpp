/// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <string_view>
#include <utility>

namespace fy::sensors {

inline constexpr std::array<std::pair<std::string_view,std::string_view>,21> amsr3_channels{{
    {"06V","06"},{"06H","06"},{"07V","07"},{"07H","07"},
    {"10uV","10u"},{"10uH","10u"},{"10V","10"},{"10H","10"},
    {"18V","18"},{"18H","18"},{"23V","23"},{"23H","23"},
    {"36V","36"},{"36H","36"},{"89AV","89A"},{"89AH","89A"},
    {"89BV","89B"},{"89BH","89B"},{"165V","165"},
    {"183r3V","183r3"},{"183r7V","183r7"}
}};

}
