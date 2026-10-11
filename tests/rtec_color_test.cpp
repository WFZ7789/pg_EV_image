/// SPDX-License-Identifier: MIT

#include "modes_fy4.h"

int main() {
    vector<float> blue{0.8f, 0.12f, 0.12f, 0.5f, 0.5f, 0.5f, 1.0f, 1.5f, 0.2f};
    vector<float> red{0.8f, 0.04f, 0.08f, 0.5f, 0.5f, 0.5f, 1.0f, 1.5f, 0.2f};
    vector<float> nir{0.8f, 0.015f, 0.4f, 0.5f, 0.5f, 0.5f, 1.0f, 1.5f, 0.2f};
    vector<float> solar{0, 0, 0, 89.9f, 90, NAN, 0, 0, 0};
    vector<float> view(blue.size(), 0);
    rayleigh_and_trc_correction_AGRI(blue, red, nir, solar, view, "FY-4B");
    int check = 0;
    const auto require = [&](bool valid) {
        ++check;
        if (!valid) throw runtime_error("FY4B 日间颜色检查失败: " + to_string(check));
    };
    require(blue[0] >= red[0] && abs(red[0] - blue[0]) < 0.20f && abs(nir[0] - blue[0]) < 0.20f);
    require(blue[1] > nir[1] && nir[1] > red[1]);
    require(max({blue[1], nir[1], red[1]}) < 0.20f);
    require(abs(blue[2] - 0.182347f) < 1e-6f &&
            abs(red[2] - 0.016217992f) < 1e-6f &&
            abs(nir[2] - 0.15927358f) < 1e-6f);
    require(isfinite(blue[3]) && isfinite(red[3]) && isfinite(nir[3]));
    require(red[6] < red[7] && red[7] < 0.99f);
    require(blue[6] < blue[7] && blue[7] < 0.99f);
    require(blue[8] >= red[8]);
    for (int i : {4, 5})
        require(isnan(blue[i]) && isnan(red[i]) && isnan(nir[i]));
}
