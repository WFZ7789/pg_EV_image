#include "pipeline.hpp"
#include "legacy_bridge.hpp"
#include "config.h"
#include "read.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <sstream>

namespace fy {
namespace {

constexpr double pi = 3.14159265358979323846;

struct Bounds {
    double left = std::numeric_limits<double>::infinity();
    double right = -std::numeric_limits<double>::infinity();
    double bottom = std::numeric_limits<double>::infinity();
    double top = -std::numeric_limits<double>::infinity();
    void add(double x, double y) {
        left = std::min(left, x); right = std::max(right, x);
        bottom = std::min(bottom, y); top = std::max(top, y);
    }
};

using Point = ProjectedPoint;

double unwrap(double longitude, double center) {
    return center + std::remainder(longitude - center, 360.0);
}

using Projection = GeoProjection;

struct Canvas {
    Bounds bounds;
    int width;
    int height;
    int left_margin = 90;
    int top_margin = 105;
    int right_margin = 180;
    int bottom_margin = 105;
    cv::Point2f pixel(Point p) const {
        return {float(left_margin + (p.x - bounds.left) * (width - 1) / (bounds.right - bounds.left)),
                float(top_margin + (bounds.top - p.y) * (height - 1) / (bounds.top - bounds.bottom))};
    }
    int total_width() const { return left_margin + width + right_margin; }
    int total_height() const { return top_margin + height + bottom_margin; }
};

bool inside(cv::Point2f p, cv::Point2f a, cv::Point2f b, cv::Point2f c) {
    float d1 = (p.x-a.x)*(b.y-a.y)-(p.y-a.y)*(b.x-a.x);
    float d2 = (p.x-b.x)*(c.y-b.y)-(p.y-b.y)*(c.x-b.x);
    float d3 = (p.x-c.x)*(a.y-c.y)-(p.y-c.y)*(a.x-c.x);
    return (d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0);
}

void paint_scene(cv::Mat3b& image, cv::Mat1i& priority, const Canvas& canvas,
                 const Projection& projection, const Scene& scene) {
    const auto& source = scene.image;
    const int rows = source.color.rows, cols = source.color.cols;
    std::vector<Point> projected(source.color.values.size());
    for (size_t i = 0; i < projected.size(); ++i)
        projected[i] = projection.project(source.geo.latitude.values[i], source.geo.longitude.values[i]);
    auto vertex = [&](int r, int c) -> Point {
        Point value{0, 0, true}; int count = 0;
        for (int dr = -1; dr <= 0; ++dr)
            for (int dc = -1; dc <= 0; ++dc) {
                int rr = std::clamp(r + dr, 0, rows - 1);
                int cc = std::clamp(c + dc, 0, cols - 1);
                const auto& point = projected[size_t(rr) * cols + cc];
                if (!point.valid) return {};
                value.x += point.x; value.y += point.y; ++count;
            }
        value.x /= count; value.y /= count; return value;
    };
    std::vector<Point> upper(cols + 1), lower(cols + 1);
    for (int c = 0; c <= cols; ++c) upper[c] = vertex(0, c);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c <= cols; ++c) lower[c] = vertex(r + 1, c);
        for (int c = 0; c < cols; ++c) {
            if (!source.valid.values.empty() && !source.valid.values[size_t(r) * cols + c]) continue;
            Point q[4]{upper[c], upper[c+1], lower[c+1], lower[c]};
            if (!q[0].valid || !q[1].valid || !q[2].valid || !q[3].valid) continue;
            cv::Point2f v[4]{canvas.pixel(q[0]), canvas.pixel(q[1]),
                             canvas.pixel(q[2]), canvas.pixel(q[3])};
            float minx = std::min({v[0].x,v[1].x,v[2].x,v[3].x});
            float maxx = std::max({v[0].x,v[1].x,v[2].x,v[3].x});
            float miny = std::min({v[0].y,v[1].y,v[2].y,v[3].y});
            float maxy = std::max({v[0].y,v[1].y,v[2].y,v[3].y});
            if (maxx - minx > 24 || maxy - miny > 24 || maxx - minx < 0.01 || maxy - miny < 0.01)
                continue;
            int x0 = std::max(canvas.left_margin, int(std::floor(minx)));
            int x1 = std::min(canvas.left_margin + canvas.width - 1, int(std::ceil(maxx)));
            int y0 = std::max(canvas.top_margin, int(std::floor(miny)));
            int y1 = std::min(canvas.top_margin + canvas.height - 1, int(std::ceil(maxy)));
            int score = std::min(c, cols - 1 - c);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    cv::Point2f p(float(x) + .5f, float(y) + .5f);
                    if (score > priority(y,x) &&
                        (inside(p,v[0],v[1],v[2]) || inside(p,v[0],v[2],v[3]))) {
                        priority(y,x) = score;
                        image(y,x) = source.color.values[size_t(r) * cols + c];
                    }
                }
        }
        upper.swap(lower);
    }
}

std::string lat_label(double v) {
    std::ostringstream out; out << std::abs(int(std::round(v))) << (v < 0 ? "S" : "N");
    return out.str();
}

std::string lon_label(double v) {
    double n = std::remainder(v, 360.0);
    std::ostringstream out; out << std::abs(int(std::round(n))) << (n < 0 ? "W" : "E");
    return out.str();
}

std::chrono::sys_time<std::chrono::minutes> parse_time(const std::string& key) {
    if (key.size() != 12) throw std::invalid_argument("无法解析观测时间: " + key);
    int year = std::stoi(key.substr(0,4));
    unsigned month = unsigned(std::stoi(key.substr(4,2)));
    unsigned day = unsigned(std::stoi(key.substr(6,2)));
    int hour = std::stoi(key.substr(8,2));
    int minute = std::stoi(key.substr(10,2));
    std::chrono::year_month_day date{std::chrono::year(year),std::chrono::month(month),std::chrono::day(day)};
    if (!date.ok() || hour > 23 || minute > 59)
        throw std::invalid_argument("观测时间无效: " + key);
    return std::chrono::sys_days(date)+std::chrono::hours(hour)+std::chrono::minutes(minute);
}

std::string display_time(const std::string& key) {
    if (key.size() != 12) return key;
    return key.substr(0,4)+"-"+key.substr(4,2)+"-"+key.substr(6,2)+" " +
           key.substr(8,2)+":"+key.substr(10,2);
}

void draw_grid(cv::Mat3b& image, const Canvas& canvas, const Projection& projection,
               double requested_step) {
    double span = projection.orthographic ? 180.0 :
        std::max(canvas.bounds.right-canvas.bounds.left, canvas.bounds.top-canvas.bounds.bottom);
    const double choices[]{1,2,5,10,15,20,30,45,60};
    double step = requested_step;
    if (step <= 0) {
        step = 60;
        for (double candidate : choices)
            if (candidate >= span / 8) { step = candidate; break; }
    }
    cv::Rect region(canvas.left_margin, canvas.top_margin, canvas.width, canvas.height);
    cv::Mat3b layer = image.clone();
    auto trace = [&](bool meridian, double fixed) {
        double begin = meridian ? -90 : projection.center_lon - 180;
        double end = meridian ? 90 : projection.center_lon + 180;
        if (!projection.orthographic) {
            begin = meridian ? canvas.bounds.bottom : canvas.bounds.left;
            end = meridian ? canvas.bounds.top : canvas.bounds.right;
        }
        int count = std::max(100, int((end-begin)*5));
        cv::Point previous; bool has_previous = false;
        for (int i = 0; i <= count; ++i) {
            double varying = begin + (end-begin)*i/count;
            Point p = meridian ? projection.project(varying, fixed) : projection.project(fixed, varying);
            if (!p.valid) { has_previous = false; continue; }
            auto point = canvas.pixel(p);
            cv::Point current(cvRound(point.x), cvRound(point.y));
            bool in = region.contains(current);
            if (has_previous && in && cv::norm(current-previous) < 25)
                cv::line(layer, previous, current, {220,220,220}, 1, cv::LINE_AA);
            previous = current; has_previous = in;
        }
    };
    double start_lon = projection.orthographic ? projection.center_lon - 180 : canvas.bounds.left;
    double end_lon = projection.orthographic ? projection.center_lon + 180 : canvas.bounds.right;
    for (double lon = std::ceil(start_lon/step)*step; lon <= end_lon; lon += step) trace(true,lon);
    for (double lat = std::ceil((projection.orthographic ? -90 : canvas.bounds.bottom)/step)*step;
         lat <= (projection.orthographic ? 90 : canvas.bounds.top); lat += step) trace(false,lat);
    cv::addWeighted(layer, .55, image, .45, 0, image);
    if (!projection.orthographic) {
        for (double lon = std::ceil(canvas.bounds.left/step)*step; lon <= canvas.bounds.right; lon += step) {
            auto pt = canvas.pixel({lon, canvas.bounds.bottom, true});
            cv::putText(image, lon_label(lon), {cvRound(pt.x)-18, canvas.top_margin+canvas.height+28},
                        cv::FONT_HERSHEY_SIMPLEX, .55, {255,255,255}, 1, cv::LINE_AA);
        }
        for (double lat = std::ceil(canvas.bounds.bottom/step)*step; lat <= canvas.bounds.top; lat += step) {
            auto pt = canvas.pixel({canvas.bounds.left, lat, true});
            cv::putText(image, lat_label(lat), {8, cvRound(pt.y)+4},
                        cv::FONT_HERSHEY_SIMPLEX, .55, {255,255,255}, 1, cv::LINE_AA);
        }
    } else {
        std::vector<cv::Rect> placed;
        for (double lon = std::ceil((projection.center_lon-80)/step)*step;
             lon <= projection.center_lon+80; lon += step) {
            auto p = projection.project(projection.center_lat,lon);
            if (!p.valid) continue;
            auto pt = canvas.pixel(p);
            std::string label = lon_label(lon);
            cv::Rect box(cvRound(pt.x)-20,cvRound(pt.y)+8,55,22);
            bool overlaps = std::any_of(placed.begin(),placed.end(),
                [&](const cv::Rect& other) { return (box & other).area() > 0; });
            if (region.contains(cv::Point(cvRound(pt.x),cvRound(pt.y))) && !overlaps) {
                cv::putText(image,label,box.tl()+cv::Point(0,15),
                            cv::FONT_HERSHEY_SIMPLEX,.5,{255,255,255},1,cv::LINE_AA);
                placed.push_back(box);
            }
        }
        for (double lat = -60; lat <= 60; lat += step) {
            auto p = projection.project(lat, projection.center_lon-80);
            if (!p.valid) continue;
            auto pt = canvas.pixel(p);
            cv::Rect box(cvRound(pt.x),cvRound(pt.y)-18,48,22);
            bool overlaps = std::any_of(placed.begin(),placed.end(),
                [&](const cv::Rect& other) { return (box & other).area() > 0; });
            if (region.contains(cv::Point(cvRound(pt.x),cvRound(pt.y))) && !overlaps) {
                cv::putText(image, lat_label(lat), box.tl()+cv::Point(0,15),
                            cv::FONT_HERSHEY_SIMPLEX,.5,{255,255,255},1,cv::LINE_AA);
                placed.push_back(box);
            }
        }
    }
}

void validate(const Request& request) {
    if (request.inputs.empty() || request.mode.empty()) throw std::invalid_argument("缺少输入或产品模式");
    if (request.width < 256 || request.width > 16000) throw std::invalid_argument("地图宽度必须在 256 至 16000 之间");
    if (request.projection != "geographic" && request.projection != "orthographic")
        throw std::invalid_argument("投影仅支持 geographic 或 orthographic");
    if (request.center_lat < -90 || request.center_lat > 90)
        throw std::invalid_argument("中心纬度必须在 -90 至 90 之间");
    if (request.half_range && (*request.half_range <= 0 || *request.half_range > 90))
        throw std::invalid_argument("区域半宽必须在 0 至 90 度之间");
    if (request.grid_step < 0 || request.grid_step > 90)
        throw std::invalid_argument("经纬线间距必须在 0 至 90 度之间");
}

}

ProjectedPoint GeoProjection::project(double lat, double lon) const {
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90 || lat > 90)
        return {};
    if (!orthographic) return {unwrap(lon, center_lon), lat, true};
    double p = lat * pi / 180, p0 = center_lat * pi / 180;
    double dl = std::remainder(lon - center_lon, 360.0) * pi / 180;
    double visibility = std::sin(p0) * std::sin(p) + std::cos(p0) * std::cos(p) * std::cos(dl);
    if (visibility < 0) return {};
    return {std::cos(p) * std::sin(dl),
            std::cos(p0) * std::sin(p) - std::sin(p0) * std::cos(p) * std::cos(dl), true};
}

void inspect(const std::filesystem::path& data) {
    H5::H5File file(data.string(), H5F_ACC_RDONLY);
    std::cout << "Satellite: " << read_str_attr(file.openGroup("/").openAttribute("Satellite Name")) << '\n';
    std::cout << "Sensor: " << read_str_attr(file.openAttribute("Sensor Identification Code")) << '\n';
    std::cout << "Alias: " << read_str_attr(file.openGroup("/").openAttribute("File Alias Name")) << '\n';
    std::cout << "Data: " << data << '\n';
}

void render(const Request& request) {
    validate(request);
    struct Entry { Input input; std::string satellite, sensor, time, resolution; };
    std::vector<Entry> entries;
    Bounds bounds;
    double global_min = std::numeric_limits<double>::infinity();
    double global_max = -std::numeric_limits<double>::infinity();
    bool percentile_mode = request.mode == "RT" || request.mode == "RY" || request.mode == "RMUL";
    std::vector<uint64_t> histogram(percentile_mode ? 65536 : 0);
    uint64_t sample_count = 0;
    double anchor = request.center_lon;
    bool has_anchor = request.half_range.has_value() || request.projection == "orthographic";
    Projection projection{request.projection == "orthographic", request.center_lat, anchor};
    for (const Input& input : request.inputs) {
        Scene scene = read_mersi(input, request);
        if (request.mode == "E")
            for (float value : scene.image.values)
                if (std::isfinite(value)) {
                    global_min = std::min(global_min, double(value));
                    global_max = std::max(global_max, double(value));
                }
        if (percentile_mode)
            for (float value : scene.image.values)
                if (std::isfinite(value)) {
                    size_t bin = size_t(std::clamp(double(value),0.0,8.0) * 65535.0 / 8.0);
                    ++histogram[bin]; ++sample_count;
                }
        std::string name = input.data.filename().string();
        std::smatch resolution_match;
        std::string resolution;
        if (std::regex_search(name, resolution_match, std::regex(R"(_([0-9]{4}M)_)")))
            resolution = resolution_match[1].str();
        if (request.inputs.size() > 1 && (scene.time_key.empty() || resolution.empty()))
            throw std::invalid_argument("拼接需要文件名中的日期、时间和分辨率: " + name);
        if (!entries.empty() && (entries.front().satellite != scene.satellite ||
            entries.front().sensor != scene.sensor || entries.front().resolution != resolution))
            throw std::invalid_argument("拼接输入的卫星、载荷或分辨率不一致: " + name);
        entries.push_back({input, scene.satellite, scene.sensor, scene.time_key, resolution});
        if (!has_anchor) {
            for (size_t i = 0; i < scene.image.geo.longitude.values.size(); ++i)
                if (std::isfinite(scene.image.geo.latitude.values[i]) && std::isfinite(scene.image.geo.longitude.values[i]) &&
                    scene.image.geo.latitude.values[i] >= -90 && scene.image.geo.latitude.values[i] <= 90) {
                    anchor = scene.image.geo.longitude.values[i]; has_anchor = true; projection.center_lon = anchor; break;
                }
        }
        for (size_t i = 0; i < scene.image.geo.latitude.values.size(); ++i) {
            Point p = projection.project(scene.image.geo.latitude.values[i], scene.image.geo.longitude.values[i]);
            if (p.valid) bounds.add(p.x,p.y);
        }
    }
    if (!std::isfinite(bounds.left)) throw std::runtime_error("输入没有有效地理定位像元");
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return std::tie(a.time,a.input.data) < std::tie(b.time,b.input.data);
    });
    for (size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].time == entries[i-1].time)
            throw std::invalid_argument("拼接存在重复观测时间: " + entries[i].time);
        auto gap = parse_time(entries[i].time)-parse_time(entries[i-1].time);
        if (gap > std::chrono::minutes(10))
            std::cerr << "提示: 轨段时间间隔为 " << gap.count() << " 分钟，未覆盖区域将留空\n";
    }
    if (request.projection == "orthographic") {
        double radius = request.half_range ? std::sin(*request.half_range * pi / 180) : 1.0;
        bounds = {-radius,radius,-radius,radius};
    } else if (request.half_range) {
        bounds = {anchor-*request.half_range,anchor+*request.half_range,
                  std::max(-90.0,request.center_lat-*request.half_range),
                  std::min(90.0,request.center_lat+*request.half_range)};
    }
    if (bounds.right-bounds.left <= 0 || bounds.top-bounds.bottom <= 0 ||
        bounds.right-bounds.left > 180)
        throw std::invalid_argument("地图范围无效或跨越超过 180 度，请指定区域中心与半宽");
    int height = projection.orthographic ? request.width :
        std::max(256, int(std::round(request.width * (bounds.top-bounds.bottom)/(bounds.right-bounds.left))));
    if (height > 16000) throw std::invalid_argument("地图高度超过 16000 像素，请缩小区域或宽度");
    Canvas canvas{bounds,request.width,height};
    cv::Mat3b image(canvas.total_height(),canvas.total_width(),cv::Vec3b(0,0,0));
    cv::Mat1i priority(canvas.total_height(),canvas.total_width(),-1);
    DisplayScale shared_scale;
    if (request.mode == "E") {
        if (!std::isfinite(global_min)) throw std::runtime_error("产品没有有效数值");
        shared_scale = {float(std::max(global_min,-105.0)),float(std::min(global_max,40.0))};
    }
    Request tuned_request = request;
    if (percentile_mode) {
        if (!sample_count) throw std::runtime_error("产品没有有效反射率像元");
        uint64_t rank = (sample_count * 99 + 99) / 100;
        uint64_t total = 0;
        for (size_t bin = 0; bin < histogram.size(); ++bin) {
            total += histogram[bin];
            if (total >= rank) {
                if (bin == histogram.size()-1)
                    throw std::runtime_error("共同拉伸范围超过 8，请检查输入产品");
                tuned_request.forced_scale = std::max(0.001f,float(bin * 8.0 / 65535.0));
                break;
            }
        }
    }
    std::function<cv::Vec3b(double,const DisplayScale&)> legend_mapper;
    DisplayScale legend_scale;
    for (const Entry& entry : entries) {
        Scene scene = read_mersi(entry.input, tuned_request);
        if (request.mode == "E") {
            if (scene.image.values.size() != scene.image.color.values.size() || !scene.image.mapper)
                throw std::runtime_error("无法统一亮温显示范围");
            for (size_t i = 0; i < scene.image.color.values.size(); ++i)
                scene.image.color.values[i] = std::isfinite(scene.image.values[i]) ?
                    scene.image.mapper(scene.image.values[i],shared_scale) : cv::Vec3b(0,0,0);
            scene.image.values.clear();
            scene.image.scale = shared_scale;
        }
        if (!legend_mapper && scene.image.has_colorbar) {
            legend_mapper = scene.image.mapper;
            legend_scale = scene.image.scale;
        }
        std::cout << "绘制 " << entry.input.data << '\n';
        paint_scene(image,priority,canvas,projection,scene);
    }
    if (request.grid) draw_grid(image,canvas,projection,request.grid_step);
    if (legend_mapper) {
        int x = canvas.left_margin + canvas.width + 35;
        int y0 = canvas.top_margin, y1 = y0 + canvas.height - 1;
        for (int y = y0; y <= y1; ++y) {
            double t = double(y-y0) / std::max(1,y1-y0);
            double value = legend_scale.hival*(1-t)+legend_scale.lowval*t;
            cv::line(image,{x,y},{x+30,y},cv::Scalar(legend_mapper(value,legend_scale)));
        }
        cv::rectangle(image,{x,y0},{x+30,y1},{255,255,255});
        cv::putText(image,std::to_string(int(std::round(legend_scale.hival))),{x+38,y0+16},
                    cv::FONT_HERSHEY_SIMPLEX,.6,{255,255,255},1,cv::LINE_AA);
        cv::putText(image,std::to_string(int(std::round(legend_scale.lowval))),{x+38,y1},
                    cv::FONT_HERSHEY_SIMPLEX,.6,{255,255,255},1,cv::LINE_AA);
    }
    std::string title = entries.front().satellite + " " + request.mode;
    if (!entries.front().time.empty()) {
        title += " " + display_time(entries.front().time);
        if (entries.size() > 1) title += " - " + display_time(entries.back().time);
    }
    cv::putText(image,title,{canvas.left_margin,52},cv::FONT_HERSHEY_SIMPLEX,1,
                {255,255,255},2,cv::LINE_AA);
    auto path = request.output.empty() ? std::filesystem::path("output/result.png") : request.output;
    if (path.extension().empty()) path += ".png";
    auto ext = path.extension().string();
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tif" && ext != ".tiff")
        throw std::invalid_argument("不支持的输出格式: " + ext);
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    if (!cv::imwrite(path.string(),image)) throw std::runtime_error("图片写入失败: " + path.string());
    std::cout << "已保存 " << path << " (" << image.cols << " x " << image.rows << ")\n";
}

}
