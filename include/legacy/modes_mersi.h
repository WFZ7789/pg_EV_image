#ifndef PG_EV_IMAGE_MODES_MERSI_H
#define PG_EV_IMAGE_MODES_MERSI_H

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

/// @brief 纬度热力图调试模式
inline void mode_TSG(Context& ctx) {
    RenderConfig cfg{-90, 90};
    vector<hsize_t> dims{1, (hsize_t) ctx.scln, (hsize_t) ctx.pps};
    rend(dims, ctx.lat, ctx.lat, ctx.lon, ctx.out_name, {"", "", "", "", ""},
         "", [cfg] (double val, const RenderConfig&) { return color_plasma(val, cfg); }, cfg,
         ctx.crop, ctx.crop_lat(), ctx.crop_lon(), ctx.target_square_size);
}

/// @brief 热辐射模式：单波段亮温渲染
inline void mode_E(Context& ctx) {
    if (kSatConfigs.find(ctx.sat) == kSatConfigs.end())
        throw invalid_argument("不支持的卫星: " + ctx.sat);
    const auto& cfgsat = kSatConfigs.at(ctx.sat).bands.at(ctx.band_id);
    auto [mat, emissive] = read_emissive(ctx.file, cfgsat, ctx.sat, ctx.isQkm);
    for (auto& i : mat.first)
        i += K2c;
    auto mmx = minmax_element(mat.first.begin(), mat.first.end());

    RenderConfig cfg;
    cfg.lowval = max(*mmx.first, -105.0f);
    cfg.hival = min(*mmx.second, 40.0f);

    auto qcolor = pick_color(ctx.color_scheme, color_rbtop_wv);
    if (active_scene_capture) active_scene_capture->values = mat.first;
    vector<cv::Vec3b> comp(mat.first.size());
    for (size_t i = 0; i < mat.first.size(); ++i)
        comp[i] = isnan(mat.first[i]) ? cv::Vec3b(0, 0, 0) : qcolor(mat.first[i], cfg);
    vector<float>().swap(mat.first);
    rend_cl(comp, ctx.lat, ctx.lon, ctx.out_name, read_labels(ctx.file, emissive, true), "",
            mat.second[1], mat.second[2], qcolor, cfg,
            ctx.crop, ctx.crop_lat(), ctx.crop_lon(), ctx.target_square_size);
}

/// @brief 反射率模式：单波段反射率渲染
inline void mode_R(Context& ctx) {
    auto solarZ = read_solar_zenith(ctx.geofile, ctx.isQkm, ctx.scln, ctx.pps);
    double Des = read_earth_sun_distance(ctx.file);
    const auto& cfgsat = kSatConfigs.at(ctx.sat).bands.at(ctx.band_id);
    auto [mat, refl] = read_reflectance_configured(ctx.file, cfgsat,
                                                   get_viscal_name(ctx.sat), ctx.isQkm);
    apply_solar_correction(mat.first, solarZ.first, Des, ctx.sat);
    RenderConfig cfg;
    cfg.lowval = 0;
    //cfg.hival = clamp<float>(percentile(mat.first, 99), 1.0, 2.0);
    cfg.hival = 2;
    auto qcolor = pick_color(ctx.color_scheme, color_enhanced_vis);
    vector<cv::Vec3b> comp(mat.first.size());
    for (size_t i = 0; i < mat.first.size(); ++i)
        comp[i] = isnan(mat.first[i]) ? cv::Vec3b(0, 0, 0) : qcolor(mat.first[i], cfg);
    vector<float>().swap(mat.first);
    vector<float>().swap(solarZ.first);
    rend_cl(comp, ctx.lat, ctx.lon, ctx.out_name, read_labels(ctx.file, refl, true), "",
            mat.second[1], mat.second[2], qcolor, cfg,
            ctx.crop, ctx.crop_lat(), ctx.crop_lon(), ctx.target_square_size);
}

/// @brief 微光产品模式 (仅 FY-3E)
inline void mode_LL(Context& ctx) {
    if (ctx.sat != "FY-3E")
        throw invalid_argument("不支持的卫星: " + ctx.sat);
    DataSet ds_ll = ctx.file.openDataSet("Data/EV_1KM_LL");
    auto mat = rer2D_base(ds_ll);
    auto mmx = minmax_element(mat.first.begin(), mat.first.end());
    mat.second.insert(mat.second.begin(), 0);
    RenderConfig render_cfg{*mmx.first, percentile(mat.first, 99)};
    rend(mat.second, mat.first, ctx.lat, ctx.lon, ctx.out_name,
         read_labels(ctx.file, ds_ll, true), "", color_refsb, render_cfg,
         ctx.crop, ctx.crop_lat(), ctx.crop_lon(), ctx.target_square_size);
}

/// @brief 假真彩色
inline void mode_RT(Context& ctx) {
    if (ctx.sat != "FY-3F" && ctx.sat != "FY-3H" && ctx.sat != "FY-3D")
        throw invalid_argument("不支持的卫星: " + ctx.sat);

    const auto& cfg_sat = kSatConfigs.at(ctx.sat);
    int blueband = 1;
    int greenband = 2;
    int redband = 3;

    auto solarZ = read_solar_zenith(ctx.geofile, ctx.isQkm, ctx.scln, ctx.pps);
    double Des = read_earth_sun_distance(ctx.file);

    auto [blue, refl1] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(blueband), get_viscal_name(ctx.sat), ctx.isQkm);
    auto [green, refl2] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(greenband), get_viscal_name(ctx.sat), ctx.isQkm);
    auto [red, refl3] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(redband), get_viscal_name(ctx.sat), ctx.isQkm);
    auto dims = red.second;
    int rows = dims[1];
    int cols = dims[2];
    size_t N = red.first.size();

    apply_solar_correction(blue.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(green.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);

    float dvf = percentile(blue.first, 99);
    if (active_scene_capture) active_scene_capture->values = blue.first;
    if (active_forced_scale) dvf = *active_forced_scale;
    vector<cv::Vec3b> comp(N);
    for (size_t i = 0; i < N; ++i)
    {
        float resb = blue.first[i], resg = green.first[i], resr = red.first[i];
        resb /= dvf;
        resg /= dvf;
        resr /= dvf;
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }

    rend_cl(comp, ctx.lat, ctx.lon, ctx.out_name, read_labels(ctx.file, refl1, false), "", rows, cols,
            nullptr, {}, ctx.crop, ctx.crop_lat(), ctx.crop_lon(), ctx.target_square_size);
}

/// @brief 自定义多波段假彩色模式
inline void mode_RY(Context& ctx) {
    if (ctx.sat != "FY-3F" && ctx.sat != "FY-3H" && ctx.sat != "FY-3D" &&
        ctx.sat != "FY-3G")
        throw invalid_argument("不支持的卫星: " + ctx.sat);

    const auto& cfg_sat = kSatConfigs.at(ctx.sat);
    int redband = 3;
    int swirband = redband + 1;
    int icyband = 6;
    int wvband = 17;
    int cirband = 5;
    int ldrefband = 7;
    int b103band = 19;
    if (ctx.sat == "FY-3G")
    {
        redband = 1;
        swirband = 2;
        icyband = 5;
        wvband = 3;
        cirband = 4;
    }

    auto solarZ = read_solar_zenith(ctx.geofile, ctx.isQkm, ctx.scln, ctx.pps);
    double Des = read_earth_sun_distance(ctx.file);

    auto [red, refl1] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(redband), get_viscal_name(ctx.sat), ctx.isQkm);
    auto [swir, refl2] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(swirband), get_viscal_name(ctx.sat), ctx.isQkm);
    auto [icy, refl3] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(icyband), get_viscal_name(ctx.sat), ctx.isQkm);
    // auto [sixteen, refl4] =
    //      read_reflectance_configured(ctx.file, cfg_sat.bands.at(wvband), get_viscal_name(ctx.sat), ctx.isQkm);
    // auto [cirrus, refl5] =
    //      read_reflectance_configured(ctx.file, cfg_sat.bands.at(cirband), get_viscal_name(ctx.sat), ctx.isQkm);
    // auto [ldref, refl6] =
    //      read_reflectance_configured(ctx.file, cfg_sat.bands.at(ldrefband), get_viscal_name(ctx.sat), ctx.isQkm);
    // auto [b103, refl7] =
    //      read_reflectance_configured(ctx.file, cfg_sat.bands.at(b103band), get_viscal_name(ctx.sat), ctx.isQkm);
    auto dims = red.second;
    int rows = dims[1];
    int cols = dims[2];
    size_t N = red.first.size();

    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(swir.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(icy.first, solarZ.first, Des, ctx.sat);
    // apply_solar_correction(sixteen.first, solarZ.first, Des, ctx.sat);
    // apply_solar_correction(cirrus.first, solarZ.first, Des, ctx.sat);
    // apply_solar_correction(ldref.first, solarZ.first, Des, ctx.sat);
    // apply_solar_correction(b103.first, solarZ.first, Des, ctx.sat);

    float dvf = percentile(red.first, 99);
    if (active_scene_capture) active_scene_capture->values = red.first;
    if (active_forced_scale) dvf = *active_forced_scale;
    vector<cv::Vec3b> comp(N);
    for (size_t i = 0; i < N; ++i)
    {
        float r = red.first[i], sir = swir.first[i];
        //float c = cirrus.first[i];
        float icyb = icy.first[i];
        //float wvvis = sixteen.first[i], ldr = ldref.first[i];
        //float b1030n = b103.first[i];
        float resb = r, resg = sir, resr = icyb;
        resb /= dvf;
        resg /= dvf;
        resr /= dvf;
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }

    rend_cl(comp, ctx.lat, ctx.lon, ctx.out_name,
            read_labels(ctx.file, refl1, true), "", rows, cols,
            nullptr, {}, ctx.crop, ctx.crop_lat(), ctx.crop_lon(), ctx.target_square_size);
}

/// @brief  假彩色模式：反射率  + 热红外融合
inline void mode_RF(Context& ctx) {
    if (ctx.sat != "FY-3F" && ctx.sat != "FY-3H" && ctx.sat != "FY-3D" && ctx.sat != "FY-3G")
        throw invalid_argument("不支持的卫星: " + ctx.sat);

    const auto& cfg_sat = kSatConfigs.at(ctx.sat);
    int redband = ctx.sat == "FY-3G" ? 1 : 3;
    int swirband = redband + 1;
    int emsvband = ctx.sat == "FY-3G" ? 7 : 24;

    auto solarZ = read_solar_zenith(ctx.geofile, ctx.isQkm, ctx.scln, ctx.pps);
    double Des = read_earth_sun_distance(ctx.file);

    auto [red, refl1] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(redband), get_viscal_name(ctx.sat), ctx.isQkm);
    auto [swir, refl2] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(swirband), get_viscal_name(ctx.sat), ctx.isQkm);
    auto [irv, emsv] =
            read_emissive(ctx.file, cfg_sat.bands.at(emsvband), ctx.sat, ctx.isQkm);

    auto dims = red.second;
    int rows = dims[1];
    int cols = dims[2];
    size_t N = red.first.size();

    for (auto& i : irv.first)
        i += K2c;
    apply_solar_correction(red.first, solarZ.first, Des, ctx.sat);
    apply_solar_correction(swir.first, solarZ.first, Des, ctx.sat);

    auto sorted = new float[N];
    copy(irv.first.begin(), irv.first.end(), sorted);
    sort(sorted, sorted + N, greater());
    int ntopt = N / 96;
    float maxt = 0;
    int startt = 0;
    while (startt < N && sorted[startt] > 30)
        ++startt;
    for (int i = 0; i < ntopt; i++)
        maxt += sorted[startt + i];
    maxt /= ntopt;
    delete[] sorted;
    float meanrat = 0, cnt = 0;
    for (size_t i = 0; i < N; ++i)
        if (!std::isnan(red.first[i]) && !std::isnan(irv.first[i]))
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
        float r = red.first[i], sir = swir.first[i];
        float irb = (maxt - irv.first[i]) / meanrat;
        float resb = irb, resg = cosh(1.3 * sir) - 1, resr = r * r;
        resr = max<float>(0, 1 - sqrt(r) + r) * resr + max<float>(0, sqrt(r) - r) * r;
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }

    rend_cl(comp, ctx.lat, ctx.lon, ctx.out_name, read_labels(ctx.file, refl1, false), "", rows, cols,
            nullptr, {}, ctx.crop, ctx.crop_lat(), ctx.crop_lon(), ctx.target_square_size);
}

/// @brief  增强型单波段反射率模式
inline void mode_RMUL(Context& ctx) {
    if (ctx.sat == "FY-3E")
        throw invalid_argument("不支持的卫星: " + ctx.sat);

    const auto& cfg_sat = kSatConfigs.at(ctx.sat);
    auto solarZ = read_solar_zenith(ctx.geofile, ctx.isQkm, ctx.scln, ctx.pps);
    double Des = read_earth_sun_distance(ctx.file);

    auto [refs, refl1] =
            read_reflectance_configured(ctx.file, cfg_sat.bands.at(ctx.band_id), get_viscal_name(ctx.sat), ctx.isQkm);

    auto dims = refs.second;
    int rows = ctx.scln;
    int cols = ctx.pps;
    size_t N = refs.first.size();

    apply_solar_correction(refs.first, solarZ.first, Des, ctx.sat);

    float mxval = percentile(refs.first, 99);
    if (active_scene_capture) active_scene_capture->values = refs.first;
    if (active_forced_scale) mxval = *active_forced_scale;

    vector<cv::Vec3b> comp(N);
    for (size_t i = 0; i < N; ++i)
    {
        if (isnan(refs.first[i]))
        {
            comp[i] = {0, 0, 0};
            continue;
        }
        float val = refs.first[i] / mxval;
        float resb = sqrt(max(0.0, val - 0.1)), resg = val - 0.05, resr = pow(val, 3);
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }

    rend_cl(comp, ctx.lat, ctx.lon, ctx.out_name, read_labels(ctx.file, refl1, true), "", rows, cols,
            nullptr, {}, ctx.crop, ctx.crop_lat(), ctx.crop_lon(), ctx.target_square_size);
}

/// @brief EC 模式：多红外波段差分模拟可见光
inline void mode_EC(Context& ctx) {
    if (ctx.sat != "FY-3F" && ctx.sat != "FY-3H" && ctx.sat != "FY-3D" && ctx.sat != "FY-3E" &&
        ctx.sat != "FY-3G")
        throw invalid_argument("不支持的卫星: " + ctx.sat);
    if (ctx.isQkm)
        throw invalid_argument("模式 EC 不支持 250M 数据");
    const auto& cfg_sat = kSatConfigs.at(ctx.sat);
    int mwirband = 20;
    int lir1band = 24;
    if (ctx.sat == "FY-3E")
    {
        mwirband = 2;
        lir1band = 6;
    }
    else if (ctx.sat == "FY-3G")
    {
        mwirband = 6;
        lir1band = 7;
    }
    int refclband = lir1band - 1;
    int lir2band = lir1band + 1;
    if (ctx.sat == "FY-3G" || ctx.sat == "FY-3F" || ctx.sat == "FY-3H")
        refclband = lir2band;

    auto [mwir, emsv] =
            read_emissive(ctx.file, cfg_sat.bands.at(mwirband), ctx.sat, ctx.isQkm);
    auto refcl =
            read_emissive(ctx.file, cfg_sat.bands.at(refclband), ctx.sat, ctx.isQkm).first;
    auto lir1 =
            read_emissive(ctx.file, cfg_sat.bands.at(lir1band), ctx.sat, ctx.isQkm).first;
    auto lir2 =
            read_emissive(ctx.file, cfg_sat.bands.at(lir2band), ctx.sat, ctx.isQkm).first;
    auto solarZ = read_solar_zenith(ctx.geofile, ctx.isQkm, ctx.scln, ctx.pps);
    auto solarA = read_solar_azimuth(ctx.geofile);

    for (auto& i : mwir.first)
        i += K2c;
    for (auto& i : refcl.first)
        i += K2c;
    for (auto& i : lir1.first)
        i += K2c;
    for (auto& i : lir2.first)
        i += K2c;

    auto trans = [&] (float val) {
        return (25 - val) / 115;
    };
    cv::Mat shade = apply_directional_shadow_fast(lir1.first, solarZ.first, solarA.first,
                                                  lir1.second[1], lir1.second[2]);
    vector<float> mmxv(lir1.first.size());
    array<vector<float>, 3> compbgr;
    compbgr.fill(vector<float>(lir1.first.size(), 0));
    for (size_t i = 0; i < lir1.first.size(); ++i)
    {
        float &resr = compbgr[2][i], &resg = compbgr[1][i],
                        &resb = compbgr[0][i];
        resr = trans(refcl.first[i]);
        resg = trans(lir1.first[i]);
        float mwircoeff = 0.5 * tanh(lir1.first[i] * 0.1 + 5) + 0.5;
        resb = trans(mwircoeff * mwir.first[i]
                     + (1 - mwircoeff) * lir2.first[i]);
        resb = 0.36 * cos(9 * resb - 1.5) + 0.6 * resb + 0.12;
        float gain = -shade.at<float>(i);
        float shade_coeff = pow((resr + resg + resb) / 3, 3);
        float shadow = shade_coeff * gain;
        resr += shadow;
        resb += shadow;
        resg += shadow;
        mmxv[i] = max({resr, resb, resg});
    }
    float dvref = max<float>(percentile(mmxv, 98), 1);
    vector<cv::Vec3b> comp(lir1.first.size());
    for (int i = 0; i < 3; i++)
        for (auto& j : compbgr[i])
            j /= dvref;
    for (int i = 0; i < lir1.first.size(); i++)
    {
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(compbgr[0][i] * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(compbgr[1][i] * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(compbgr[2][i] * 255, 0, 255))
        };
    }
    rend_cl(comp, ctx.lat, ctx.lon, ctx.out_name, read_labels(ctx.file, emsv, true), "", lir1.second[1],
            lir1.second[2], nullptr, {}, ctx.crop, ctx.crop_lat(), ctx.crop_lon(),
            ctx.target_square_size);
}

/// @brief  伪VIS 变体模式：水汽 + 多红外差分
inline void mode_EK(Context& ctx) {
    if (ctx.sat != "FY-3F" && ctx.sat != "FY-3H" && ctx.sat != "FY-3D" && ctx.sat != "FY-3E")
        throw invalid_argument("不支持的卫星:\t" + ctx.sat);
    if (ctx.isQkm)
        throw invalid_argument("模式 EK 不支持 250M 数据");
    const auto& cfg_sat = kSatConfigs.at(ctx.sat);
    int mwirband = 20;
    int lir1band = 24;
    int fireband = mwirband + 1;
    if (ctx.sat == "FY-3E")
    {
        mwirband = 2;
        fireband = 3;
        lir1band = 6;
    }
    int wvband = lir1band - 2;
    int lir2band = lir1band + 1;

    auto [mwir, emsv] =
            read_emissive(ctx.file, cfg_sat.bands.at(mwirband), ctx.sat, ctx.isQkm);
    auto waterv =
            read_emissive(ctx.file, cfg_sat.bands.at(wvband), ctx.sat, ctx.isQkm).first;
    auto lir1 =
            read_emissive(ctx.file, cfg_sat.bands.at(lir1band), ctx.sat, ctx.isQkm).first;
    auto lir2 =
            read_emissive(ctx.file, cfg_sat.bands.at(lir2band), ctx.sat, ctx.isQkm).first;
    auto solarZ = read_solar_zenith(ctx.geofile, ctx.isQkm, ctx.scln, ctx.pps);
    auto solarA = read_solar_azimuth(ctx.geofile);

    for (auto& i : mwir.first)
        i += K2c;
    for (auto& i : waterv.first)
        i += K2c;
    for (auto& i : lir1.first)
        i += K2c;
    for (auto& i : lir2.first)
        i += K2c;
    vector<float> subt(lir1.first.size());
    auto trans_lir1 = [&] (float val) {
        return (25 - val) / 115;
    };
    auto trans_wv = [&] (float val) {
        return (-35 - val) / 55;
    };
    float mwirhi = percentile(mwir.first, 98) + 3;
    float lir1hi = percentile(lir1.first, 98) + 4;
    float lwdiff = mwirhi - lir1hi;
    for (int i = 0; i < lir1.first.size(); i++)
    {
        float coeff = (tanh(0.1 * lir1.first[i] + 4) + 1) * 0.5;
        float mwira = mwir.first[i] * coeff + lir2.first[i] * (1 - coeff);
        subt[i] = sqrt(max<float>((mwirhi - mwira) / (lir1hi - lir1.first[i]) * 0.5, 0));
    }

    vector<float> srt(lir1.first.size());
    for (int i = 0; i < srt.size(); i++)
    {
        srt[i] = trans_lir1(lir2.first[i]);
    }
    cv::Mat shade = apply_directional_shadow_fast(lir1.first, solarZ.first, solarA.first,
                                                  lir1.second[1], lir1.second[2]);
    vector<float> visv(lir1.first.size());
    for (size_t i = 0; i < lir1.first.size(); ++i)
    {
        float coeff = (tanh(0.1 * lir1.first[i] + 5) + 1) * 0.5;
        float val = coeff * subt[i] + (1 - coeff) * srt[i];
        float gain = -shade.at<float>(i);
        float shade_coeff = pow(val, 3);
        float shadow = shade_coeff * gain;
        val += shadow;
        visv[i] = val;
    }
    float black_value = percentile(visv, 3);
    for (size_t i = 0; i < lir1.first.size(); ++i)
        visv[i] -= black_value;
    float dvref = percentile(visv, 96);
    vector<cv::Vec3b> comp(lir1.first.size());
    for (int i = 0; i < lir1.first.size(); i++)
    {
        visv[i] /= dvref;
        float resg, resr, resb;
        resg = resb = resr = visv[i];
        comp[i] = {
            static_cast<uint8_t>(clamp<int>(resb * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resg * 255, 0, 255)),
            static_cast<uint8_t>(clamp<int>(resr * 255, 0, 255))
        };
    }
    rend_cl(comp, ctx.lat, ctx.lon, ctx.out_name, read_labels(ctx.file, emsv, true), "", lir1.second[1],
            lir1.second[2], nullptr, {}, ctx.crop, ctx.crop_lat(), ctx.crop_lon(),
            ctx.target_square_size);
}

/// @brief 模式调度
inline void dispatch(Context& ctx, const string& mode) {
    if (mode == "TSG") mode_TSG(ctx);
    else if (mode == "E") mode_E(ctx);
    else if (mode == "R") mode_R(ctx);
    else if (mode == "LL") mode_LL(ctx);
    else if (mode == "RT") mode_RT(ctx);
    else if (mode == "RY") mode_RY(ctx);
    else if (mode == "RF") mode_RF(ctx);
    else if (mode == "RMUL") mode_RMUL(ctx);
    else if (mode == "EC") mode_EC(ctx);
    else if (mode == "EK") mode_EK(ctx);
    else throw invalid_argument("不存在模式\t'" + mode + "'");
}

#endif
