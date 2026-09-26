/// SPDX-License-Identifier: MIT

#ifndef PG_EV_IMAGE_CONTEXT_H
#define PG_EV_IMAGE_CONTEXT_H

#include "stl.h"
#include "phys.h"

using namespace std;
using namespace H5;

struct Context {
    H5File& file;
    H5File& geofile;
    string aux_file;
    string sat;
    string sen_code;
    string out_name;
    int band_id = 0;
    double target_lat = 0;
    double target_lon = 0;
    string color_scheme;
    bool crop = false;
    bool aq = false;
    float target_square_size = 7.5;
    bool isQkm = false;
    bool ec_aux_qkm = false;
    vector<float> lat;
    vector<float> lon;
    int scln = 0;
    int pps = 0;

    /// @brief 裁剪中心：正交投影（--aq）下以投影中心 (0,0) 为原点
    double crop_lat() const { return aq ? 0.0 : target_lat; }
    double crop_lon() const { return aq ? 0.0 : target_lon; }
};

/// @brief  对反射率进行太阳校正：除以 100（非 FY-3G）并乘以 Des²/cos(SZA)
inline void apply_solar_correction(vector<float>& refl, const vector<float>& solarZ, double Des, const string& sat) {
    if (sat != "FY-3G" && sat.substr(0, 4) != "FY-4")
        for (auto& i : refl)
            i /= 1e2;
    for (size_t i = 0; i < refl.size(); i++)
        refl[i] *= static_cast<float>(pow(Des, 2)) / static_cast<float>(cos(solarZ[i] * Deg2rad));
}

#endif
