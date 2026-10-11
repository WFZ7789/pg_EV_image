/// SPDX-License-Identifier: MIT

#include "legacy_bridge.hpp"
#include "modes.h"

#include <regex>

namespace fy {

namespace {

std::string observation_start(const H5::H5File& file) {
    const auto root = file.openGroup("/");
    return read_str_attr(root.openAttribute("Observing Beginning Date")) + " " +
           read_str_attr(root.openAttribute("Observing Beginning Time"));
}

}

Scene read_mersi(const Input& input, const Request& request) {
    if (request.inputs.size() > 1 && input.geo.empty())
        throw std::invalid_argument("拼接任务必须为每段显式提供 GEO: " + input.data.string());
    H5::H5File file(input.data.string(), H5F_ACC_RDONLY);
    H5::H5File geo((input.geo.empty() ? input.data : input.geo).string(), H5F_ACC_RDONLY);
    const auto sat = read_str_attr(file.openGroup("/").openAttribute("Satellite Name"));
    const auto sensor = read_str_attr(file.openAttribute("Sensor Identification Code"));
    if (sat.rfind("FY-3", 0) != 0 || sensor.find("MERSI") == std::string::npos)
        throw std::invalid_argument("输入不是 FY-3 MERSI: " + input.data.string());
    if ((request.mode == "E" || request.mode == "R") &&
        (!kSatConfigs.contains(sat) || !kSatConfigs.at(sat).bands.contains(request.band)))
        throw std::invalid_argument("卫星 " + sat + " 不支持波段 " + std::to_string(request.band));
    const auto alias = read_str_attr(file.openGroup("/").openAttribute("File Alias Name"));
    std::smatch match;
    std::string filename = input.data.filename().string();
    std::string time_key;
    if (std::regex_search(filename, match, std::regex(R"((20[0-9]{6})_([0-9]{4}))")))
        time_key = match[1].str() + match[2].str();
    if (request.inputs.size() > 1) {
        std::smatch geo_match;
        std::string geo_name = input.geo.filename().string();
        if (!std::regex_search(geo_name,geo_match,std::regex(R"((20[0-9]{6})_([0-9]{4}))")) ||
            time_key != geo_match[1].str()+geo_match[2].str())
            throw std::invalid_argument("数据与 GEO 的观测时间不匹配: " + input.data.string());
    }
    const bool qkm = alias.find("250M") != std::string::npos || alias.find("QKM") != std::string::npos;
    bool ec_aux_qkm = false;
    int rows = read_u16_attr(file.openAttribute("Scan_Line_number"));
    int cols = read_u16_attr(file.openAttribute("Pixels_per_Scan"));
    if (request.mode == "EC" && !input.aux.empty()) {
        if (qkm)
            throw std::invalid_argument("EC 使用 250M 附加数据时，主数据必须是同轨 1KM 文件");
        H5::H5File aux(input.aux.string(), H5F_ACC_RDONLY);
        const auto aux_sat = read_str_attr(aux.openGroup("/").openAttribute("Satellite Name"));
        const auto aux_sensor = read_str_attr(aux.openAttribute("Sensor Identification Code"));
        const auto aux_alias = read_str_attr(aux.openGroup("/").openAttribute("File Alias Name"));
        if (aux_sat != sat || aux_sensor != sensor)
            throw std::invalid_argument("EC 的 250M 附加数据与主数据卫星或载荷不一致");
        if (observation_start(aux) != observation_start(file))
            throw std::invalid_argument("EC 的 250M 附加数据与主数据观测时间不一致");
        if (aux_alias.find("250M") == std::string::npos && aux_alias.find("QKM") == std::string::npos)
            throw std::invalid_argument("EC 的附加数据必须是 250M 文件");
        const int lir1_band = sat == "FY-3E" ? 6 : sat == "FY-3G" ? 7 : 24;
        const int lir2_band = lir1_band + 1;
        if (!kSatConfigs.contains(sat) || !kSatConfigs.at(sat).bands.contains(lir1_band) ||
            !kSatConfigs.at(sat).bands.contains(lir2_band) ||
            kSatConfigs.at(sat).bands.at(lir1_band).qkdataset.empty() ||
            kSatConfigs.at(sat).bands.at(lir2_band).qkdataset.empty())
            throw std::invalid_argument("卫星 " + sat + " 的 EC 不支持 250M 10.8/12.0um 附加数据");
        rows = read_u16_attr(aux.openAttribute("Scan_Line_number"));
        cols = read_u16_attr(aux.openAttribute("Pixels_per_Scan"));
        ec_aux_qkm = true;
    }
    const auto latitude = geo.openDataSet("Geolocation/Latitude");
    const auto longitude = geo.openDataSet("Geolocation/Longitude");
    if ((qkm || ec_aux_qkm) && sat == "FY-3D") cols = 8192;
    std::vector<float> lat, lon;
    if (qkm || ec_aux_qkm) {
        lat = read_and_interp_geo_float(geo, latitude, rows, cols);
        lon = read_and_interp_geo_float(geo, longitude, rows, cols, true);
    } else {
        lat = read_geolocation(latitude).first;
        lon = read_geolocation(longitude).first;
    }
    if (lat.size() != lon.size() || lat.size() != size_t(rows) * cols)
        throw std::invalid_argument("数据与 GEO 网格尺寸不一致: " + input.data.string());
    Context ctx{file, geo, input.aux.string(), sat, sensor, "", request.band,
                request.center_lat, request.center_lon};
    ctx.color_scheme = request.color;
    ctx.isQkm = qkm;
    ctx.ec_aux_qkm = ec_aux_qkm;
    ctx.lat = std::move(lat);
    ctx.lon = std::move(lon);
    ctx.scln = rows;
    ctx.pps = cols;
    CapturedScene image;
    struct CaptureGuard {
        std::optional<float> previous_scale;
        CaptureGuard(CapturedScene& destination, const std::optional<float>& scale) {
            active_scene_capture = &destination;
            previous_scale = active_forced_scale;
            active_forced_scale = scale;
        }
        ~CaptureGuard() { active_scene_capture = nullptr; active_forced_scale = previous_scale; }
    } guard(image,request.forced_scale);
    dispatch(ctx, request.mode);
    if (!image.rows || image.pixels.size() != size_t(image.rows) * image.cols ||
        image.lat.size() != image.pixels.size() || image.lon.size() != image.pixels.size())
        throw std::runtime_error("产品输出与地理定位网格尺寸不一致: " + input.data.string());
    ProductFrame frame;
    frame.color = {image.rows,image.cols,std::move(image.pixels)};
    frame.geo = {{image.rows,image.cols,std::move(image.lat)},
                 {image.rows,image.cols,std::move(image.lon)}};
    frame.valid = {image.rows,image.cols,std::move(image.valid)};
    frame.values = std::move(image.values);
    frame.scale = {image.scale.lowval,image.scale.hival};
    frame.has_colorbar = image.has_colorbar;
    if (image.mapper) {
        auto mapper = std::move(image.mapper);
        frame.mapper = [mapper=std::move(mapper)](double value,const DisplayScale& scale) {
            return mapper(value,RenderConfig{scale.lowval,scale.hival});
        };
    }
    frame.validate();
    return {std::move(frame), sat, sensor, alias, time_key, input.data,
            std::move(image.labels), std::move(image.extra_label)};
}

}
