#include "legacy_bridge.hpp"
#include "modes.h"

#include <regex>

namespace fy {

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
    const auto latitude = geo.openDataSet("Geolocation/Latitude");
    const auto longitude = geo.openDataSet("Geolocation/Longitude");
    int rows = read_u16_attr(file.openAttribute("Scan_Line_number"));
    int cols = read_u16_attr(file.openAttribute("Pixels_per_Scan"));
    if (qkm && sat == "FY-3D") cols = 8192;
    std::vector<float> lat, lon;
    if (qkm) {
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
    return {std::move(frame), sat, sensor, alias, time_key, input.data};
}

}
