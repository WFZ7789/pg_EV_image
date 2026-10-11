/// SPDX-License-Identifier: MIT

#ifndef PG_EV_IMAGE_MODES_FY4_H
#define PG_EV_IMAGE_MODES_FY4_H

#include "stl.h"
#include "phys.h"
#include "config.h"
#include "color.h"
#include "read.h"
#include "rend.h"
#include "draw.h"
#include "context.h"

using namespace std;
using namespace H5;

/// @brief 检查 FY-4B 全圆盘数据是否支持区域渲染
inline void validate_fy4_crop(Context& ctx, int rows, int cols) {
    if (!ctx.crop)
        return;
    if (ctx.sat != "FY-4B" || rows != cols || rows != ctx.scln || cols != ctx.pps)
        throw invalid_argument("区域渲染目前仅支持 FY-4B 全圆盘模式");
}

/// @brief 渲染 FY-4B AGRI 单通道数据并按需裁剪
template<typename QColorFn>
inline void rend_fy4_matrix(Context& ctx, const vector<hsize_t>& dims,
                             const vector<float>& mat, string out_name,
                             const vector<string>& lbls, const string& es_label,
                             QColorFn qcolor, const RenderConfig& cfg) {
    int rows = static_cast<int>(dims.at(0));
    int cols = static_cast<int>(dims.at(1));
    if (!ctx.crop)
    {
        rend_matrix(dims, mat, std::move(out_name), lbls, es_label, qcolor, cfg);
        return;
    }
    validate_fy4_crop(ctx, rows, cols);
    vector<cv::Vec3b> comp(mat.size());
    for (size_t i = 0; i < mat.size(); ++i)
        comp[i] = isnan(mat[i]) ? cv::Vec3b(0, 0, 0) : qcolor(mat[i], cfg);
    auto [lat, lon] = read_fy4_latlon(ctx.file, rows, cols);
    rend_cl(comp, lat, lon, std::move(out_name), lbls, es_label, rows, cols,
            qcolor, cfg, true, ctx.target_lat, ctx.target_lon, ctx.target_square_size);
}

/// @brief 渲染 FY-4B AGRI 彩色数据并按需裁剪
inline void rend_fy4_cl(Context& ctx, const vector<cv::Vec3b>& comp,
                         string out_name, const vector<string>& lbls,
                         const string& es_label, int rows, int cols) {
    if (!ctx.crop)
    {
        rend_cl_matrix(comp, std::move(out_name), lbls, es_label, rows, cols);
        return;
    }
    validate_fy4_crop(ctx, rows, cols);
    auto [lat, lon] = read_fy4_latlon(ctx.file, rows, cols);
    rend_cl(comp, lat, lon, std::move(out_name), lbls, es_label, rows, cols,
            nullptr, {}, true, ctx.target_lat, ctx.target_lon, ctx.target_square_size);
}

/// @brief 渲染 FY-4B AGRI 16 位彩色数据并按需裁剪
inline void rend_fy4_cl_16(Context& ctx, const vector<cv::Vec<uint16_t, 3>>& comp,
                            string out_name, const vector<string>& lbls,
                            const string& es_label, int rows, int cols) {
    if (!ctx.crop)
    {
        rend_cl_matrix_16(comp, std::move(out_name), lbls, es_label, rows, cols);
        return;
    }
    validate_fy4_crop(ctx, rows, cols);
    auto [lat, lon] = read_fy4_latlon(ctx.file, rows, cols);
    rend_cl_16(comp, lat, lon, std::move(out_name), lbls, es_label, rows, cols,
               true, ctx.target_lat, ctx.target_lon, ctx.target_square_size);
}

/// @brief 表观反射率模式：单波段反射率渲染
inline void mode_RE_fy4(Context& ctx) {
    auto [mat, refl] =
            read_fy4(ctx.file, ctx.band_id);
    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    double Des = 1;
    if (ctx.sen_code == "GHI")
        Des = read_earth_sun_distance(ctx.file, "Earth_Sun Distance Ratio");
    else
        Des = read_earth_sun_distance(ctx.file, "Earth/Sun Distance Ratio");
    apply_solar_correction(mat.first, solarZ.first, Des, ctx.sat);
    RenderConfig cfg{0, 2};
    rend_fy4_matrix(ctx, mat.second, mat.first, ctx.out_name,
                    read_labels(ctx.file, refl, true), "", color_enhanced_vis, cfg);
}

/// @brief 反射率模式：单波段反射率渲染
inline void mode_R_fy4(Context& ctx) {
    auto [mat, refl] =
            read_fy4(ctx.file, ctx.band_id);
    RenderConfig cfg;
    cfg.lowval = 0;
    cfg.hival = clamp<float>(percentile(mat.first, 99.9), 1.0, 2.0);
    rend_fy4_matrix(ctx, mat.second, mat.first, ctx.out_name,
                    read_labels(ctx.file, refl, true), "", color_refsb, cfg);
}

/// @brief 热辐射模式：单波段亮温渲染
inline void mode_E_fy4(Context& ctx) {
    auto [mat, emissive] =
            read_fy4(ctx.file, ctx.band_id);
    for (auto& i : mat.first)
        i += K2c;
    RenderConfig cfg{-105, 40};
    rend_fy4_matrix(ctx, mat.second, mat.first, ctx.out_name,
                    read_labels(ctx.file, emissive, true), "", color_rbtop_wv, cfg);
}

/// @brief EC 模式：多红外波段差分模拟可见光
inline void mode_EC_fy4(Context& ctx) {
    int mwirband = 7;
    int lir1band = 13;
    int refclband = 12;
    int lir2band = lir1band + 1;

    auto [mwir, emsv] =
            read_fy4(ctx.file, mwirband);
    auto refcl =
            read_fy4(ctx.file, refclband).first;
    auto lir1 =
            read_fy4(ctx.file, lir1band).first;
    auto lir2 =
            read_fy4(ctx.file, lir2band).first;
    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    auto solarA = read_solar_azimuth_fy4(ctx.geofile, ctx.scln, ctx.pps);

    for (auto& i : mwir.first)
        i += K2c;
    for (auto& i : refcl.first)
        i += K2c;
    for (auto& i : lir1.first)
        i += K2c;

    auto trans = [&] (float val) {
        return (25 - val) / 115;
    };
    auto trans_mwir = [&] (float val) {
        return (25 - val) / 65;
    };
    cv::Mat shade = apply_directional_shadow_fast(lir1.first, solarZ.first, solarA.first,
                                                  lir1.second[0], lir1.second[1]);
    vector<float> mmxv(lir1.first.size());
    vector<array<float, 3>> compbgr(lir1.first.size());
    for (size_t i = 0; i < lir1.first.size(); ++i)
    {
        float &resr = compbgr[i][2], &resg = compbgr[i][1],
                &resb = compbgr[i][0];
        float mwircoeff = 0.7 * tanh(lir1.first[i] * 0.1 + 5) + 0.5;
        resb = trans(mwircoeff * mwir.first[i]
                     + (1 - mwircoeff) * lir2.first[i]);
        resr = trans(refcl.first[i]);
        resg = trans(lir1.first[i]);
        resb = 0.36 * cos(9 * resb - 1.5) + 0.6 * resb + 0.12;
        float gain = -shade.at<float>(i) / 3;
        float shade_coeff = pow((resr + resg + resb) / 3, 3);
        float shadow = shade_coeff * gain;
        resr += shadow;
        resb += shadow;
        resg += shadow;
        mmxv[i] = max({resr, resb, resg});
    }
    float dvref = max<float>(percentile(mmxv, 98), 1);
    vector<cv::Vec3b> comp(lir1.first.size());
    for (int i = 0; i < lir1.first.size(); i++)
    {
        if (isnan(compbgr[i][0]) || isnan(compbgr[i][1]) || isnan(compbgr[i][2]))
            continue;
        for (auto& j : compbgr[i])
            j /= dvref;
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(compbgr[i][0] * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(compbgr[i][1] * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(compbgr[i][2] * 255, 0, 255))
        };
    }
    rend_fy4_cl(ctx, comp, ctx.out_name, read_labels(ctx.file, emsv, false), "", ctx.scln,
                ctx.pps);
}

/// @brief 假彩色模式 反射率 + 热红外融合
inline void mode_RF_fy4(Context& ctx) {
    int redband = 2;
    int swirband = 3;
    int emsvband = 13;
    if (ctx.sen_code == "GHI")
    {
        redband = 4;
        swirband = 1;
        emsvband = 7;
    }
    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    double Des = 1;
    if (ctx.sen_code == "GHI")
        Des = read_earth_sun_distance(ctx.file, "Earth_Sun Distance Ratio");
    else
        Des = read_earth_sun_distance(ctx.file, "Earth/Sun Distance Ratio");

    auto [red, refl1] =
            read_fy4(ctx.file, redband);
    auto [swir, refl2] =
            read_fy4(ctx.file, swirband);
    H5File irfile = ctx.file;
    if (!ctx.aux_file.empty())
        irfile.openFile(ctx.aux_file, H5F_ACC_RDONLY);
    auto [irv, emsv] =
            read_fy4(irfile, emsvband);
    if (!ctx.aux_file.empty())
        irv = interp(irv, ctx.scln, ctx.pps);

    auto dims = red.second;
    int rows = dims[0];
    int cols = dims[1];
    size_t N = red.first.size();

    for (auto& i : irv.first)
        i += K2c;
    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(swir.first, solarZ.first, Des, ctx.sat);

    double maxt = percentile(irv.first, 95);
    double meanrat = 0, cnt = 0;
    for (size_t i = 0; i < N; ++i)
        if (!std::isnan(red.first[i]) && red.first[i] > 1e-2 && !std::isnan(irv.first[i]))
        {
            meanrat += (maxt - irv.first[i]) / red.first[i];
            ++cnt;
        }
    meanrat /= cnt;

    vector<cv::Vec3b> comp(N);
    for (size_t i = 0; i < N; ++i)
    {
        if (isnan(irv.first[i]) || isnan(swir.first[i]) || isnan(red.first[i]))
        {
            comp[i] = {0, 0, 0};
            continue;
        }
        float r = clamp(red.first[i], 0.0f, 3.0f), sir = clamp(swir.first[i], 0.0f, 3.0f);
        float irb = (maxt - irv.first[i]) / meanrat;
        float resb = irb, resg = pow(sir, 1.5), resr = pow(r, 1.2);
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }

    vector<float>().swap(red.first);
    vector<float>().swap(swir.first);
    vector<float>().swap(irv.first);
    vector<float>().swap(solarZ.first);

    rend_fy4_cl(ctx, comp, ctx.out_name, read_labels(ctx.file, refl1, false), "", rows, cols);
}

inline void mode_RFALD_GHI(Context& ctx) {
    int emsvband = 7;
    int redband = 4;
    int greenband = 3;
    int blueband = 2;
    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    double Des = read_earth_sun_distance(ctx.file, "Earth_Sun Distance Ratio");
    auto [red, refl1] =
            read_fy4(ctx.file, redband);
    auto [green, refl2] =
            read_fy4(ctx.file, greenband);
    auto [blue, refl3] =
            read_fy4(ctx.file, blueband);
    auto [irv, emsv] =
            read_fy4(ctx.file, emsvband);

    auto dims = red.second;
    int rows = dims[0];
    int cols = dims[1];
    size_t N = red.first.size();

    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(green.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(blue.first, solarZ.first, Des, ctx.sat);

    for (auto& i : irv.first)
        i += K2c;

    double maxt = percentile(irv.first, 95);
    double meanrat = 115;

    vector<cv::Vec3b> comp(N);
    for (size_t i = 0; i < N; ++i)
    {
        if (isnan(irv.first[i]) || isnan(red.first[i]))
        {
            comp[i] = {0, 0, 0};
            continue;
        }
        float vb = blue.first[i],
                vg = pow(green.first[i], 1.3),
                vr = pow(red.first[i], 2);
        if (solarZ.first[i] > 90)
        {
            vb = (maxt - irv.first[i]) / 45;
            vg = vr = (maxt - irv.first[i]) / meanrat;
        }
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(vb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(vg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(vr * 255, 0, 255))
        };
    }

    rend_fy4_cl(ctx, comp, ctx.out_name, read_labels(ctx.file, refl2, false), "", rows, cols);
}

/// @brief 自然色模式
inline void mode_RY_fy4(Context& ctx) {
    int redband = 2;
    int swirband = 3;
    int icyband = 5;
    if (ctx.sen_code == "GHI")
    {
        redband = 4;
        swirband = 1;
        icyband = 6;
    }
    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    double Des = 1;
    if (ctx.sen_code == "GHI")
        Des = read_earth_sun_distance(ctx.file, "Earth_Sun Distance Ratio");
    else
        Des = read_earth_sun_distance(ctx.file, "Earth/Sun Distance Ratio");

    auto [red, refl1] =
            read_fy4(ctx.file, redband);
    auto [swir, refl2] =
            read_fy4(ctx.file, swirband);
    H5File icyfile = ctx.file;
    if (!ctx.aux_file.empty())
        icyfile.openFile(ctx.aux_file, H5F_ACC_RDONLY);
    auto [icyv, refl3] =
            read_fy4(icyfile, icyband);
    if (!ctx.aux_file.empty())
        icyv = interp(icyv, ctx.scln, ctx.pps);

    auto dims = red.second;
    int rows = dims[0];
    int cols = dims[1];
    size_t N = red.first.size();

    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(swir.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(icyv.first, solarZ.first, Des, ctx.sat);

    vector<cv::Vec3b> comp(N);
    for (size_t i = 0; i < N; ++i)
    {
        if (isnan(icyv.first[i]) || isnan(swir.first[i]) || isnan(red.first[i]))
        {
            comp[i] = {0, 0, 0};
            continue;
        }
        float resb = clamp(red.first[i], 0.0f, 3.0f),
                resg = clamp(swir.first[i], 0.0f, 3.0f),
                resr = clamp(icyv.first[i], 0.0f, 3.0f);
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }

    rend_fy4_cl(ctx, comp, ctx.out_name, read_labels(ctx.file, refl1, false), "", rows, cols);
}

/// @brief 自然色模式
inline void mode_RX_fy4(Context& ctx) {
    int redband = 2;
    int lwvband = 6;
    int icyband = 5;
    if (ctx.sen_code == "GHI")
    {
        redband = 4;
        lwvband = 1;
        icyband = 6;
    }
    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    double Des = 1;
    if (ctx.sen_code == "GHI")
        Des = read_earth_sun_distance(ctx.file, "Earth_Sun Distance Ratio");
    else
        Des = read_earth_sun_distance(ctx.file, "Earth/Sun Distance Ratio");

    auto [red, refl1] =
            read_fy4(ctx.file, redband);
    auto [lwv, refl2] =
            read_fy4(ctx.file, lwvband);
    auto [icyv, refl3] =
            read_fy4(ctx.file, icyband);

    auto dims = red.second;
    int rows = dims[0];
    int cols = dims[1];
    size_t N = red.first.size();

    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(lwv.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(icyv.first, solarZ.first, Des, ctx.sat);

    vector<cv::Vec3b> comp(N);
    for (size_t i = 0; i < N; ++i)
    {
        if (isnan(icyv.first[i]) || isnan(lwv.first[i]) || isnan(red.first[i]))
        {
            comp[i] = {0, 0, 0};
            continue;
        }
        float resb = clamp(red.first[i], 0.0f, 3.0f),
                resg = clamp(lwv.first[i] * 0.5f + red.first[i] * 0.5f, 0.0f, 3.0f),
                resr = clamp(icyv.first[i] * 0.5f + red.first[i] * 0.5f, 0.0f, 3.0f);
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }

    rend_fy4_cl(ctx, comp, ctx.out_name, read_labels(ctx.file, refl1, false), "", rows, cols);
}


/// @brief 假真彩色模式 (0.825um)
inline void mode_RTpp_fy4(Context& ctx) {
    int redband = 2;
    int greenband = 3;
    int blueband = 1;
    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    double Des = 1;
    if (ctx.sen_code == "GHI")
        Des = read_earth_sun_distance(ctx.file, "Earth_Sun Distance Ratio");
    else
        Des = read_earth_sun_distance(ctx.file, "Earth/Sun Distance Ratio");

    auto [red, refl1] =
            read_fy4(ctx.file, redband);
    auto [green, refl2] =
            read_fy4(ctx.file, greenband);
    auto [blue, refl3] =
            read_fy4(ctx.file, blueband);

    auto dims = red.second;
    int rows = dims[0];
    int cols = dims[1];
    size_t N = red.first.size();

    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(green.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(blue.first, solarZ.first, Des, ctx.sat);

    vector<cv::Vec3b> comp(N);
    for (size_t i = 0; i < N; ++i)
    {
        if (isnan(blue.first[i]) || isnan(green.first[i]) || isnan(red.first[i]))
        {
            comp[i] = {0, 0, 0};
            continue;
        }
        float resb = clamp(blue.first[i], 0.0f, 3.0f),
                resg = clamp(green.first[i], 0.0f, 3.0f),
                resr = clamp(red.first[i], 0.0f, 3.0f);
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }

    vector<float>().swap(red.first);
    vector<float>().swap(green.first);
    vector<float>().swap(blue.first);
    vector<float>().swap(solarZ.first);

    rend_fy4_cl(ctx, comp, ctx.out_name, read_labels(ctx.file, refl1, false), "", rows, cols);
}

inline void rayleigh_and_trc_correction_AGRI(vector<float>& blue, vector<float>& red,
                                             vector<float>& nir, const vector<float>& solarZ,
                                             const vector<float>& satZ, const string& sat) {
    constexpr array kRayleigh_coefs{0.1726f, 0.0501f, 0.0187f};
    constexpr float kSRF_R[3] = {-0.1992f, 1.3768f, -0.1770f};
    constexpr float kSRF_G[3] = {0.4500f, 0.4500f, 0.1000f};
    constexpr float kSRF_B[3] = {1.0000f, 0.0000f, 0.0000f};
    const size_t N = blue.size();
    for (size_t i = 0; i < N; i++)
    {
        const float raw_blue = blue[i], raw_red = red[i], raw_nir = nir[i];
        if (!isfinite(raw_blue) || !isfinite(raw_red) || !isfinite(raw_nir) ||
            !isfinite(solarZ[i]) || !isfinite(satZ[i]) ||
            solarZ[i] >= 90.0f || satZ[i] >= 90.0f)
        {
            blue[i] = red[i] = nir[i] = NAN;
            continue;
        }
        float mu_s = max(cos(solarZ[i] * static_cast<float>(Deg2rad)), 0.05f);
        float mu_v = max(cos(satZ[i] * static_cast<float>(Deg2rad)), 0.05f);
        float max_refl = max({raw_blue, raw_red, raw_nir});
        float weight = 1.0f - clamp(max_refl / 0.6f, 0.0f, 1.0f);
        float rpath[3];
        for (int c = 0; c < 3; c++)
        {
            float tau = kRayleigh_coefs[c];
            rpath[c] = (1.0f - exp(-tau / mu_s - tau / mu_v)) /
                       (2.0f * (1.0f / mu_s + 1.0f / mu_v));
        }
        const float clean_blue = max(raw_blue - rpath[0] * weight, 0.0f);
        const float clean_red = max(raw_red - rpath[1] * weight, 0.0f);
        const float clean_nir = max(raw_nir - rpath[2] * weight, 0.0f);
        const float output_red = kSRF_R[0] * clean_blue + kSRF_R[1] * clean_red + kSRF_R[2] * clean_nir;
        const float output_green = kSRF_G[0] * clean_blue + kSRF_G[1] * clean_red + kSRF_G[2] * clean_nir;
        const float output_blue = kSRF_B[0] * clean_blue + kSRF_B[1] * clean_red + kSRF_B[2] * clean_nir;
        red[i] = pow(max(output_red, 0.0f), sat == "FY-4B" ? 0.87f : 0.85f) *
                 (sat == "FY-4B" ? 0.90f : 0.90873f);
        nir[i] = pow(max(output_green, 0.0f), 0.85f);
        blue[i] = pow(max(output_blue, 0.0f), 0.75f) * 1.06f;
        if (sat == "FY-4B")
        {
            const float peak = max({red[i], nir[i], blue[i]});
            const float highlight_scale = peak > 0.82f
                    ? (0.82f + 0.16f * (1.0f - exp(-(peak - 0.82f) / 0.16f))) / peak
                    : 1.0f;
            red[i] *= highlight_scale;
            nir[i] *= highlight_scale;
            blue[i] *= highlight_scale;
        }
    }
}

inline auto rayleigh_correction_GHI(hsize_t N, const vector<float>& blue,
                                    const vector<float>& green, const vector<float>& red,
                                    const vector<float>& solarZ,
                                    const vector<float>& satZ) {
    constexpr static array kRayleigh_coefs{0.1858f, 0.1006f, 0.0496f};
    array rclean{vector<float>(N), vector<float>(N), vector<float>(N)};
    for (size_t i = 0; i < N; i++)
    {
        float mu_s = cos(solarZ[i] * static_cast<float>(Deg2rad));
        float mu_v = cos(satZ[i] * static_cast<float>(Deg2rad));
        float max_refl = max({blue[i], green[i], red[i]});
        float weight = 1.0f - clamp(max_refl / 0.6f, 0.0f, 1.0f);
        float rpath[3];
        for (int c = 0; c < 3; c++)
        {
            float tau = kRayleigh_coefs[c];
            rpath[c] = (1.0f - exp(-tau / mu_s - tau / mu_v)) /
                       (2.0f * (1.0f / mu_s + 1.0f / mu_v));
        }
        rclean[0][i] = max(blue[i] - rpath[0] * weight, 0.0f);
        rclean[1][i] = max(green[i] - rpath[1] * weight, 0.0f);
        rclean[2][i] = max(red[i] - rpath[2] * weight, 0.0f);
    }
    return rclean;
}

struct RtecInfraredBands {
    int mwir;
    int refcl;
    int lir1;
    int lir2;
};

/// @brief 返回与卫星通道定义匹配的 RTEC 红外波段
inline RtecInfraredBands rtec_infrared_bands(const string& sat) {
    if (sat == "FY-4A")
        return {7, 11, 12, 13};
    if (sat == "FY-4B")
        return {7, 12, 13, 14};
    throw invalid_argument("RTEC 暂未配置 " + sat + " 的红外通道");
}

inline float rtec_smoothstep(float edge0, float edge1, float value) {
    const float x = clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

inline void apply_rtec_solar_correction(vector<float>& red, vector<float>& green,
                                        vector<float>& blue, const vector<float>& solar_zenith,
                                        double earth_sun_distance) {
    const float distance_scale = static_cast<float>(earth_sun_distance * earth_sun_distance);
    cv::parallel_for_(cv::Range(0, static_cast<int>(red.size())), [&](const cv::Range& range) {
        for (int i = range.start; i < range.end; ++i)
        {
            if (isnan(solar_zenith[i]) || solar_zenith[i] >= 90.0f)
            {
                red[i] = green[i] = blue[i] = NAN;
                continue;
            }
            const float mu = max(cos(solar_zenith[i] * static_cast<float>(Deg2rad)), 0.05f);
            const float scale = distance_scale / mu;
            red[i] *= scale;
            green[i] *= scale;
            blue[i] *= scale;
        }
    });
}

template<typename Channel, int MaxValue>
inline void compose_rtec(vector<cv::Vec<Channel, 3>>& comp,
                         const vector<float>& blue, const vector<float>& green,
                         const vector<float>& red, const vector<float>& mwir,
                         const vector<float>& refcl, const vector<float>& lir1,
                         const vector<float>& lir2, const vector<float>& solar_zenith,
                         const cv::Mat& shade) {
    const auto trans = [](float value) {
        return (25.0f - value) / 115.0f;
    };
    const int rows = shade.rows;
    const int cols = shade.cols;
    cv::parallel_for_(cv::Range(0, rows), [&](const cv::Range& range) {
        for (int row = range.start; row < range.end; ++row)
        {
            float sst_latest = 20.0f;
            const float* shade_row = shade.ptr<float>(row);
            for (int col = 0; col < cols; ++col)
            {
        const size_t i = static_cast<size_t>(row) * cols + col;
        const bool night_valid = !isnan(lir1[i]) && !isnan(lir2[i]) && !isnan(mwir[i]);
        cv::Vec3f night{0.0f, 0.0f, 0.0f};
        if (night_valid)
        {
            const float sst_update = 0.5f * tanh(0.1f * lir1[i] - 0.9f) + 0.5f;
            sst_latest = sst_update * lir1[i] + (1.0f - sst_update) * sst_latest;
            const float mwir_coeff = 0.5f * tanh(lir1[i] * 0.1f + 5.0f) + 0.5f;
            const float pvis_ldlb_coeff =
                    (1.0f - trans(lir1[i])) * 0.5f * tanh(0.15f * sst_latest - 2.0f) + 0.5f;
            const float ldlb = (26.0f - mwir[i]) / (29.0f - lir1[i]);
            const float fllb = trans(mwir_coeff * mwir[i] +
                                     (1.0f - mwir_coeff) * (lir1[i] - 7.0f));
            float resb = pvis_ldlb_coeff *
                         (mwir_coeff * ldlb + (1.0f - mwir_coeff) * trans(lir1[i])) +
                         (1.0f - pvis_ldlb_coeff) *
                         (0.36f * cos(9.0f * fllb - 1.5f) + 0.6f * fllb);
            float resr = trans(refcl[i]);
            float resg = trans(lir1[i]);
            const float low_cloud = rtec_smoothstep(0.5f, 3.0f, lir1[i] - mwir[i]) *
                                    rtec_smoothstep(-40.0f, -8.0f, lir1[i]) *
                                    rtec_smoothstep(92.0f, 98.0f, solar_zenith[i]);
            const float low_cloud_brightness = max({resr, resg, resb});
            const float low_cloud_weight = 0.28f * low_cloud;
            resb = resb * (1.0f - low_cloud_weight) +
                   min(low_cloud_brightness * 1.05f, 1.0f) * low_cloud_weight;
            resg = resg * (1.0f - low_cloud_weight) +
                   low_cloud_brightness * low_cloud_weight;
            resr = resr * (1.0f - low_cloud_weight) +
                   low_cloud_brightness * low_cloud_weight;
            const float cdo = (1.0f - rtec_smoothstep(-50.0f, -35.0f, lir1[i])) *
                              (1.0f - rtec_smoothstep(1.0f, 3.0f, abs(lir2[i] - lir1[i]))) *
                              rtec_smoothstep(90.0f, 96.0f, solar_zenith[i]);
            const float cdo_brightness = max({resr, resg, resb});
            const float cdo_weight = 0.32f * cdo;
            resb = resb * (1.0f - cdo_weight) + cdo_brightness * cdo_weight;
            resg = resg * (1.0f - cdo_weight) + cdo_brightness * cdo_weight;
            resr = resr * (1.0f - cdo_weight) + cdo_brightness * cdo_weight;
            const float ice_phase = rtec_smoothstep(0.3f, 1.2f, refcl[i] - lir1[i]);
            const float nonopaque_ice = rtec_smoothstep(0.7f, 2.5f, abs(lir1[i] - lir2[i]));
            const float cold_cloud = 1.0f - rtec_smoothstep(-32.0f, -5.0f, lir1[i]);
            const float cirrus = ice_phase * nonopaque_ice * cold_cloud * (1.0f - cdo);
            const float yellowness = rtec_smoothstep(0.03f, 0.18f, min(resr, resg) - resb);
            const float yellow_suppression = 0.85f * yellowness * (1.0f - cirrus) * (1.0f - cdo);
            const float neutral = (resr + resg + resb) / 3.0f;
            resr = resr * (1.0f - yellow_suppression) + neutral * yellow_suppression;
            resg = resg * (1.0f - yellow_suppression) + neutral * yellow_suppression;
            const float gain = -shade_row[col] / 3.0f;
            const float average = (resr + resg + resb) / 3.0f;
            const float shadow = average * average * average * gain;
            resr += shadow;
            resg += shadow;
            resb += shadow;
            night = {resb, resg, resr};
        }

        const bool day_valid = solar_zenith[i] < 90.0f && !isnan(blue[i]) &&
                               !isnan(green[i]) && !isnan(red[i]);
        cv::Vec3f color = night;
        if (day_valid)
        {
            const cv::Vec3f day{
                clamp(blue[i], 0.0f, 1.0f),
                clamp(green[i], 0.0f, 1.0f),
                clamp(red[i], 0.0f, 1.0f)
            };
            const float night_weight = night_valid
                    ? rtec_smoothstep(82.0f, 90.0f, solar_zenith[i])
                    : 0.0f;
            color = day * (1.0f - night_weight) + night * night_weight;
        }
        else if (!night_valid)
            color = {0.0f, 0.0f, 0.0f};

        comp[i] = {
            static_cast<Channel>(clamp<int64_t>(llround(color[0] * MaxValue), 0, MaxValue)),
            static_cast<Channel>(clamp<int64_t>(llround(color[1] * MaxValue), 0, MaxValue)),
            static_cast<Channel>(clamp<int64_t>(llround(color[2] * MaxValue), 0, MaxValue))
        };
        }
        }
    });
}

/// @brief RT&EC全天模式
inline void mode_RTEC_fy4(Context& ctx) {
    int redband = 2;
    int greenband = 3;
    int blueband = 1;
    const auto infrared_bands = rtec_infrared_bands(ctx.sat);

    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    auto solarA = read_solar_azimuth_fy4(ctx.geofile, ctx.scln, ctx.pps);
    auto satZ = read_satellite_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);

    double Des = read_earth_sun_distance(ctx.file, "Earth/Sun Distance Ratio");
    auto [red, refl1] =
            read_fy4(ctx.file, redband);
    auto [green, refl2] =
            read_fy4(ctx.file, greenband);
    auto [blue, refl3] =
            read_fy4(ctx.file, blueband);
    H5File irfile = ctx.file;
    if (!ctx.aux_file.empty())
        irfile.openFile(ctx.aux_file, H5F_ACC_RDONLY);
    auto mwir = read_fy4(irfile, infrared_bands.mwir).first;
    auto refcl = read_fy4(irfile, infrared_bands.refcl).first;
    auto lir1 =
            read_fy4(irfile, infrared_bands.lir1).first;
    auto lir2 = read_fy4(irfile, infrared_bands.lir2).first;
    if (!ctx.aux_file.empty())
    {
        mwir = interp(mwir, ctx.scln, ctx.pps);
        refcl = interp(refcl, ctx.scln, ctx.pps);
        lir1 = interp(lir1, ctx.scln, ctx.pps);
        lir2 = interp(lir2, ctx.scln, ctx.pps);
    }
    for (auto& i : mwir.first)
        i += K2c;
    for (auto& i : refcl.first)
        i += K2c;
    for (auto& i : lir1.first)
        i += K2c;
    for (auto& i : lir2.first)
        i += K2c;

    auto dims = red.second;
    int rows = dims[0];
    int cols = dims[1];
    size_t N = red.first.size();

    apply_rtec_solar_correction(red.first, green.first, blue.first, solarZ.first, Des);
    rayleigh_and_trc_correction_AGRI(blue.first, red.first, green.first,
                                      solarZ.first, satZ.first, ctx.sat);
    cv::Mat shade = apply_directional_shadow_fast(lir1.first, solarZ.first, solarA.first,
                                                  lir1.second[0], lir1.second[1]);
    vector<cv::Vec3b> comp(N);
    compose_rtec<uint8_t, 255>(comp, blue.first, green.first, red.first, mwir.first,
                               refcl.first, lir1.first, lir2.first, solarZ.first, shade);

    vector<float>().swap(red.first);
    vector<float>().swap(green.first);
    vector<float>().swap(blue.first);
    vector<float>().swap(mwir.first);
    vector<float>().swap(refcl.first);
    vector<float>().swap(lir1.first);
    vector<float>().swap(lir2.first);
    vector<float>().swap(solarZ.first);
    vector<float>().swap(solarA.first);
    vector<float>().swap(satZ.first);
    shade.release();

    rend_fy4_cl(ctx, comp, ctx.out_name, read_labels(ctx.file, refl1, false), "", rows, cols);
}

/// @brief RT&EC全天模式
inline void mode_RTEC16_fy4(Context& ctx) {
    int redband = 2;
    int greenband = 3;
    int blueband = 1;
    const auto infrared_bands = rtec_infrared_bands(ctx.sat);

    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    auto solarA = read_solar_azimuth_fy4(ctx.geofile, ctx.scln, ctx.pps);
    auto satZ = read_satellite_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    double Des = read_earth_sun_distance(ctx.file, "Earth/Sun Distance Ratio");
    auto [red, refl1] =
            read_fy4(ctx.file, redband);
    auto [green, refl2] =
            read_fy4(ctx.file, greenband);
    auto [blue, refl3] =
            read_fy4(ctx.file, blueband);
    H5File irfile = ctx.file;
    if (!ctx.aux_file.empty())
        irfile.openFile(ctx.aux_file, H5F_ACC_RDONLY);
    auto mwir = read_fy4(irfile, infrared_bands.mwir).first;
    auto refcl = read_fy4(irfile, infrared_bands.refcl).first;
    auto lir1 =
            read_fy4(irfile, infrared_bands.lir1).first;
    auto lir2 = read_fy4(irfile, infrared_bands.lir2).first;
    if (!ctx.aux_file.empty())
    {
        mwir = interp(mwir, ctx.scln, ctx.pps);
        refcl = interp(refcl, ctx.scln, ctx.pps);
        lir1 = interp(lir1, ctx.scln, ctx.pps);
        lir2 = interp(lir2, ctx.scln, ctx.pps);
    }
    for (auto& i : mwir.first)
        i += K2c;
    for (auto& i : refcl.first)
        i += K2c;
    for (auto& i : lir1.first)
        i += K2c;
    for (auto& i : lir2.first)
        i += K2c;

    auto dims = red.second;
    int rows = dims[0];
    int cols = dims[1];
    size_t N = red.first.size();

    apply_rtec_solar_correction(red.first, green.first, blue.first, solarZ.first, Des);
    rayleigh_and_trc_correction_AGRI(blue.first, red.first, green.first,
                                      solarZ.first, satZ.first, ctx.sat);
    cv::Mat shade = apply_directional_shadow_fast(lir1.first, solarZ.first, solarA.first,
                                                  lir1.second[0], lir1.second[1]);
    vector<cv::Vec<uint16_t, 3>> comp(N);
    compose_rtec<uint16_t, 65535>(comp, blue.first, green.first, red.first, mwir.first,
                                  refcl.first, lir1.first, lir2.first, solarZ.first, shade);

    vector<float>().swap(red.first);
    vector<float>().swap(green.first);
    vector<float>().swap(blue.first);
    vector<float>().swap(mwir.first);
    vector<float>().swap(refcl.first);
    vector<float>().swap(lir1.first);
    vector<float>().swap(lir2.first);
    vector<float>().swap(solarZ.first);
    vector<float>().swap(solarA.first);
    vector<float>().swap(satZ.first);
    shade.release();

    rend_fy4_cl_16(ctx, comp, ctx.out_name, read_labels(ctx.file, refl1, false), "", rows, cols);
}

inline void mode_RT_GHI(Context& ctx) {
    int red_band = 4;
    int green_band = 3;
    int blue_band = 2;
    int fullcolor_band = 1;

    auto solarZ = read_solar_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    auto satZ = read_satellite_zenith_fy4(ctx.geofile, ctx.scln, ctx.pps);
    double Des = read_earth_sun_distance(ctx.file, "Earth_Sun Distance Ratio");
    auto [red, refl1] =
            read_fy4(ctx.file, red_band);
    auto [green, refl2] =
            read_fy4(ctx.file, green_band);
    auto [blue, refl3] =
            read_fy4(ctx.file, blue_band);

    auto dims = red.second;
    int rows = dims[0];
    int cols = dims[1];
    size_t N = red.first.size();

    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(green.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(blue.first, solarZ.first, Des, ctx.sat);
    auto rgb = rayleigh_correction_GHI(N, blue.first, green.first, red.first,
                                       solarZ.first, satZ.first);
    for (int i = 0; i < N; i++)
    {
        rgb[0][i] = pow(rgb[0][i], 0.8) * 1.19;
        rgb[1][i] = pow(rgb[1][i], 0.8) * 1.097;
        rgb[2][i] = pow(rgb[2][i], 0.85) * 0.979;
    }
    vector<float>().swap(red.first);
    vector<float>().swap(green.first);
    vector<float>().swap(blue.first);
    vector<float>().swap(solarZ.first);
    vector<float>().swap(satZ.first);
    vector<cv::Vec3b> comp(N);
    if (!ctx.aux_file.empty())
    {
        H5File fulc_file(ctx.aux_file, H5F_ACC_RDONLY);
        auto fullcolor = read_fy4(fulc_file, fullcolor_band).first;

        auto solarZ_fullc = read_solar_zenith_fy4(ctx.geofile,
                                                  fullcolor.second[0], fullcolor.second[1]);
        apply_solar_correction(fullcolor.first, solarZ_fullc.first, Des, ctx.sat);

        for (int i = 0; i < 3; i++)
            rgb[i] = interp(rgb[i], red.second,
                            fullcolor.second[0], fullcolor.second[1]);
        comp.resize(fullcolor.first.size());
        rows = fullcolor.second[0];
        cols = fullcolor.second[1];

        for (int i = 0; i < fullcolor.first.size(); i++)
        {
            if (isnan(rgb[0][i]) || isnan(rgb[1][i]) ||
                isnan(rgb[2][i]) || isnan(fullcolor.first[i]) ||
                rgb[0][i] < 1e-3)
            {
                comp[i] = {0, 0, 0};
                continue;
            }
            float coeff = 3 * fullcolor.first[i] / (rgb[0][i] + rgb[1][i] + rgb[2][i]);
            comp[i] = {
                static_cast<uint8_t>(clamp<int>(rgb[0][i] * coeff * 255, 0, 255)),
                static_cast<uint8_t>(clamp<int>(rgb[1][i] * coeff * 255, 0, 255)),
                static_cast<uint8_t>(clamp<int>(rgb[2][i] * coeff * 255, 0, 255))
            };
        }
        vector<float>().swap(fullcolor.first);
        vector<float>().swap(solarZ_fullc.first);
    }
    else
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (isnan(rgb[0][i]) || isnan(rgb[1][i]) ||
                isnan(rgb[2][i]))
            {
                comp[i] = {0, 0, 0};
                continue;
            }
            comp[i] = {
                static_cast<uint8_t>(clamp<int>(rgb[0][i] * 255, 0, 255)),
                static_cast<uint8_t>(clamp<int>(rgb[1][i] * 255, 0, 255)),
                static_cast<uint8_t>(clamp<int>(rgb[2][i] * 255, 0, 255))
            };
        }
    }
    for (auto& channel : rgb)
        vector<float>().swap(channel);
    rend_fy4_cl(ctx, comp, ctx.out_name,
                read_labels(ctx.file, refl1, false), "", rows, cols);
}

/// @brief 模式调度
inline void dispatch_fy4(Context& ctx, const string& mode) {
    if (mode == "Rr")
        mode_R_fy4(ctx);
    else if (mode == "R" || mode == "Re")
        mode_RE_fy4(ctx);
    else if (mode == "E")
        mode_E_fy4(ctx);
    else if (mode == "EC")
        mode_EC_fy4(ctx);
    else if (mode == "RF")
        mode_RF_fy4(ctx);
    else if (mode == "RY")
        mode_RY_fy4(ctx);
    else if (mode == "RTG")
        mode_RT_GHI(ctx);
    else if (mode == "RT16" || mode == "RTEC16")
        mode_RTEC16_fy4(ctx);
    else if (mode == "RT+EC" || mode == "RTEC")
        mode_RTEC_fy4(ctx);
    else if (mode == "RTpp")
        mode_RTpp_fy4(ctx);
    else if (mode == "RX")
        mode_RX_fy4(ctx);
    else if (mode == "AGHI_ALD")
    {
        if (ctx.sen_code == "GHI")
            mode_RFALD_GHI(ctx);
        else
            mode_RTEC_fy4(ctx);
    }
    else
        throw invalid_argument("不存在模式 '" + mode + "'");
}

#endif
