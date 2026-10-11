/// SPDX-License-Identifier: MIT

#ifndef PG_EV_IMAGE_DRAW_H
#define PG_EV_IMAGE_DRAW_H

#include    "stl.h"
#include    "phys.h"

#include    <stdexcept>

struct LegacyGrid {
    bool enabled = false;
    double step = 0;
    bool orthographic = false;
    double center_lat = 0;
    double center_lon = 0;
};

/// @brief 保留旧版正交投影的运算顺序和以度表示的画布坐标。
inline std::pair<float, float> legacy_orthographic(double latitude, double longitude,
                                                 double center_lat, double center_lon) {
    center_lat *= Deg2rad;
    center_lon *= Deg2rad;
    double x = latitude * Deg2rad;
    double y = longitude * Deg2rad;
    double dist = acos(sin(center_lat) * sin(x) +
                       cos(center_lat) * cos(x) * cos(y - center_lon));
    double angle = atan2(sin(y - center_lon) * cos(x),
                        cos(center_lat) * sin(x) - sin(center_lat) * cos(x) * cos(y - center_lon));
    double R = sin(dist);
    double screen_x = R * sin(angle);
    double screen_y = R * cos(angle);
    return {static_cast<float>(screen_y / Deg2rad), static_cast<float>(screen_x / Deg2rad)};
}

/// @brief 将经纬度转换为画布坐标
inline cv::Point to_canvas(double la, double lo, double min_la, double min_lo,
                           double x_factor, double y_factor, int H, int offH) {
    int x = static_cast<int>((lo - min_lo) * x_factor);
    int y = H - 1 - static_cast<int>((la - min_la) * y_factor) - offH / 2;
    return {x, y};
}

/// @brief 仅在旧版主体地图内叠加经纬网，不改动画布布局。
inline void draw_legacy_grid(cv::Mat& canvas, const cv::Rect& region,
                             double min_la, double max_la, double min_lo, double max_lo,
                             const LegacyGrid& grid) {
    if (!grid.enabled) return;
    double step = grid.step;
    if (step <= 0) {
        step = 60;
        for (double candidate : {1., 2., 5., 10., 15., 20., 30., 45., 60.})
            if (candidate >= std::max(max_la - min_la, max_lo - min_lo) / 8) {
                step = candidate;
                break;
            }
    }
    const double xf = (region.width - 1) / (max_lo - min_lo);
    const double yf = (region.height - 1) / (max_la - min_la);
    cv::Mat layer = canvas(region).clone();
    auto trace = [&](bool meridian, double fixed) {
        double begin = meridian ? min_la : min_lo, end = meridian ? max_la : max_lo;
        if (grid.orthographic) {
            begin = meridian ? -90 : grid.center_lon - 180;
            end = meridian ? 90 : grid.center_lon + 180;
        }
        int count = std::max(100, int(std::ceil((end - begin) * std::max(xf, yf) / 4)));
        cv::Point previous;
        bool connected = false, labelled = false;
        for (int i = 0; i <= count; ++i) {
            double varying = begin + (end - begin) * i / count;
            double la = meridian ? varying : fixed, lo = meridian ? fixed : varying;
            if (grid.orthographic) {
                double visibility = sin(grid.center_lat * Deg2rad) * sin(la * Deg2rad) +
                    cos(grid.center_lat * Deg2rad) * cos(la * Deg2rad) * cos((lo - grid.center_lon) * Deg2rad);
                if (visibility < 0) { connected = false; continue; }
                auto point = legacy_orthographic(la, lo, grid.center_lat, grid.center_lon);
                la = point.first; lo = point.second;
            }
            if (!std::isfinite(la) || !std::isfinite(lo)) { connected = false; continue; }
            auto point = to_canvas(la, lo, min_la, min_lo, xf, yf, region.height, 0);
            bool in = cv::Rect(0, 0, region.width, region.height).contains(point);
            if (in && connected && cv::norm(point - previous) < 25)
                cv::line(layer, previous, point, {220, 220, 220}, 1, cv::LINE_AA);
            if (in && !labelled) {
                double value = meridian ? std::remainder(fixed, 360.) : fixed;
                std::ostringstream label;
                label << std::abs(value) << (meridian ? (value < 0 ? "W" : "E") : (value < 0 ? "S" : "N"));
                int baseline = 0;
                auto size = cv::getTextSize(label.str(), cv::FONT_HERSHEY_SIMPLEX, .9, 1, &baseline);
                cv::Point position(std::clamp(point.x + 8, 2, std::max(2, region.width - size.width - 2)),
                                   std::clamp(point.y + size.height + 8, size.height + 2,
                                              std::max(size.height + 2, region.height - baseline - 2)));
                cv::putText(layer, label.str(), position, cv::FONT_HERSHEY_SIMPLEX, .9,
                            {255, 255, 255}, 1, cv::LINE_AA);
                labelled = true;
            }
            previous = point; connected = in;
        }
    };
    double left = grid.orthographic ? grid.center_lon - 180 : min_lo;
    double right = grid.orthographic ? grid.center_lon + 180 : max_lo;
    double bottom = grid.orthographic ? -90 : min_la, top = grid.orthographic ? 90 : max_la;
    for (double lo = std::ceil(left / step) * step; lo <= right; lo += step) trace(true, lo);
    for (double la = std::ceil(bottom / step) * step; la <= top; la += step) trace(false, la);
    cv::addWeighted(layer, .55, canvas(region), .45, 0, canvas(region));
}

/// @brief 计算目标正方形区域（lat/lon ± half）在行列网格中的包围盒，
///        并返回区域内像元的地理经纬度范围（用于画布定位）
/// @return 区域内存在有效像元时返回 true
inline bool target_area_bounds(const std::vector<float>& lat, const std::vector<float>& lon,
                               int cols, double tlat, double tlon, double half,
                               int& r0, int& r1, int& c0, int& c1,
                               double& min_la, double& max_la, double& min_lo, double& max_lo) {
    r0 = INT_MAX;
    r1 = -1;
    c0 = INT_MAX;
    c1 = -1;
    min_la = DBL_MAX;
    max_la = -DBL_MAX;
    min_lo = DBL_MAX;
    max_lo = -DBL_MAX;
    for (size_t i = 0; i < lat.size(); i++)
        if (lat[i] >= tlat - half && lat[i] <= tlat + half &&
            lon[i] >= tlon - half && lon[i] <= tlon + half)
        {
            int r = static_cast<int>(i / cols), c = static_cast<int>(i % cols);
            r0 = std::min(r0, r);
            r1 = std::max(r1, r);
            c0 = std::min(c0, c);
            c1 = std::max(c1, c);
            min_la = std::min(min_la, static_cast<double>(lat[i]));
            max_la = std::max(max_la, static_cast<double>(lat[i]));
            min_lo = std::min(min_lo, static_cast<double>(lon[i]));
            max_lo = std::max(max_lo, static_cast<double>(lon[i]));
        }
    return r1 >= r0 && c1 >= c0;
}

/// @brief 按包围盒裁剪扁平数据、纬度、经度数组
template<typename T>
inline void crop_flat(const std::vector<T>& src, const std::vector<float>& lat,
                      const std::vector<float>& lon, int cols, int r0, int r1, int c0, int c1,
                      std::vector<T>& dst, std::vector<float>& dlat, std::vector<float>& dlon) {
    int nrows = r1 - r0 + 1, ncols = c1 - c0 + 1;
    dst.resize(static_cast<size_t>(nrows) * ncols);
    dlat.resize(static_cast<size_t>(nrows) * ncols);
    dlon.resize(static_cast<size_t>(nrows) * ncols);
    for (int r = r0; r <= r1; r++)
        for (int c = c0; c <= c1; c++)
        {
            int si = r * cols + c, di = (r - r0) * ncols + (c - c0);
            dst[di] = src[si];
            dlat[di] = lat[si];
            dlon[di] = lon[si];
        }
}

/// @brief  计算像元网格的顶点矩阵  (类似 matplotlib pcolormesh)
/// @return 大小为 (rows+1)*(cols+1) 的顶点数组，用于绘制无缝四边形
inline std::vector<cv::Point> compute_grid_vertices(
    int rows, int cols, const std::vector<float>& lat, const std::vector<float>& lon,
    double min_la, double min_lo, double x_factor, double y_factor, int H, int offH) {
    std::vector<cv::Point> V((rows + 1) * (cols + 1));
    auto get_pt = [&] (int r, int c) -> cv::Point {
        r = std::clamp(r, 0, rows - 1);
        c = std::clamp(c, 0, cols - 1);
        return to_canvas(lat[r * cols + c], lon[r * cols + c], min_la, min_lo, x_factor, y_factor, H, offH);
    };

    for (int r = 0; r <= rows; ++r)
    {
        for (int c = 0; c <= cols; ++c)
        {
            cv::Point p00 = get_pt(r - 1, c - 1);
            cv::Point p01 = get_pt(r - 1, c);
            cv::Point p10 = get_pt(r, c - 1);
            cv::Point p11 = get_pt(r, c);
            V[r * (cols + 1) + c] = cv::Point(
                (p00.x + p01.x + p10.x + p11.x) / 4,
                (p00.y + p01.y + p10.y + p11.y) / 4
            );
        }
    }
    return V;
}

/// @brief 计算像元网格的一行顶点，避免为整幅图保存顶点矩阵
inline void compute_grid_vertex_row(
    int row, int rows, int cols, const std::vector<float>& lat, const std::vector<float>& lon,
    double min_la, double min_lo, double x_factor, double y_factor, int H, int offH,
    std::vector<cv::Point>& vertices) {
    vertices.resize(static_cast<size_t>(cols) + 1);
    auto get_pt = [&] (int r, int c) -> cv::Point {
        r = std::clamp(r, 0, rows - 1);
        c = std::clamp(c, 0, cols - 1);
        return to_canvas(lat[static_cast<size_t>(r) * cols + c],
                         lon[static_cast<size_t>(r) * cols + c],
                         min_la, min_lo, x_factor, y_factor, H, offH);
    };
    for (int col = 0; col <= cols; ++col) {
        const cv::Point p00 = get_pt(row - 1, col - 1);
        const cv::Point p01 = get_pt(row - 1, col);
        const cv::Point p10 = get_pt(row, col - 1);
        const cv::Point p11 = get_pt(row, col);
        vertices[col] = cv::Point((p00.x + p01.x + p10.x + p11.x) / 4,
                                  (p00.y + p01.y + p10.y + p11.y) / 4);
    }
}

/// @brief 计算数组的百分位数
inline float percentile(const std::vector<float>& data, float perc) {
    std::vector<float> tmp;
    tmp.reserve(data.size());
    for (float v : data)
        if (!std::isnan(v))
            tmp.push_back(v);
    if (tmp.empty())
        return 0.0f;
    auto nth = tmp.begin() + static_cast<long>(perc / 100.0f * (tmp.size() - 1));
    std::nth_element(tmp.begin(), nth, tmp.end());
    return *nth;
}

/// @brief 矩阵平移
inline void shiftMatrix(const cv::Mat& src, cv::Mat& dst, int dx, int dy) {
    dst = cv::Mat::zeros(src.size(), src.type());

    int src_x = std::max(0, -dx);
    int src_y = std::max(0, -dy);
    int dst_x = std::max(0, dx);
    int dst_y = std::max(0, dy);
    int width = src.cols - std::abs(dx);
    int height = src.rows - std::abs(dy);

    if (width > 0 && height > 0)
        src(cv::Range(src_y, src_y + height), cv::Range(src_x, src_x + width))
                .copyTo(dst(cv::Range(dst_y, dst_y + height), cv::Range(dst_x, dst_x + width)));
}

/// @brief 使用方向性差分模拟真实太阳角度造成的阴影
inline cv::Mat apply_directional_shadow_fast(
    const std::vector<float>& lir1,
    const std::vector<float>& solarZ,
    const std::vector<float>& solarA,
    int rows, int cols) {
    cv::Mat Tb(rows, cols, CV_32F, const_cast<float*>(lir1.data()));

    double avg_sza = 0, avg_saa = 0, valid_count = 0;
    for (int i = 0; i < rows * cols; ++i)
        if (!std::isnan(solarZ[i]) && solarZ[i] > 0 && solarZ[i] < 180)
        {
            if (solarZ[i] > 90)
                avg_sza += 180 - solarZ[i];
            else
                avg_sza += solarZ[i];
            if (solarA[i] > 180)
                avg_saa += 360 - solarA[i];
            else
                avg_saa += solarA[i];
            ++valid_count;
        }

    avg_sza /= valid_count;
    avg_saa /= valid_count;

    float sun_elevation = 90 - avg_sza;
    float light_angle = avg_saa * Deg2rad;

    int offset = static_cast<int>(15.0f / tan(sun_elevation * Deg2rad));
    offset = std::clamp(offset, 1, 10);

    cv::Mat shadow_effect(rows, cols, CV_32F, cv::Scalar(0.0f));

    for (int scale = 1; scale <= offset; ++scale)
    {
        int cur_dx = static_cast<int>(cos(light_angle) * scale);
        int cur_dy = static_cast<int>(sin(light_angle) * scale);

        cv::Mat shifted_Tb;
        shiftMatrix(Tb, shifted_Tb, -cur_dx, -cur_dy);

        cv::Mat diff;
        cv::subtract(Tb, shifted_Tb, diff);

        float weight = 1.0f / static_cast<float>(scale);

        cv::Mat scale_shadow;
        cv::threshold(diff, scale_shadow, 0.0f, 0.0f, cv::THRESH_TRUNC);
        scale_shadow = scale_shadow * weight * 0.1f;

        cv::add(shadow_effect, scale_shadow, shadow_effect);
    }

    cv::threshold(shadow_effect, shadow_effect, 0.3f, 0.3f, cv::THRESH_TRUNC);

    return shadow_effect;
}

/// @brief 生成随机输出文件名
inline std::string random_output_filename() {
    std::string res(6, '\0');
    static std::random_device seed;
    static std::mt19937 rnd(seed());
    std::uniform_int_distribution<> chdis(48, 57);
    for (auto& i : res)
        i = chdis(rnd);
    return res;
}

#endif
