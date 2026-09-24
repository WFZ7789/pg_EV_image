#ifndef PG_EV_IMAGE_REND_H
#define PG_EV_IMAGE_REND_H

#include    "stl.h"
#include    "config.h"
#include    "draw.h"

#include    <opencv2/imgproc.hpp>
#include    <opencv2/opencv.hpp>

#define OUTPUT_DIR  "./output/"
//#define OUTNAME_EQ_FILENAME
//#define TIFF_OUTPUT
#define JPEG_OUTPUT

using namespace std;

#ifdef TIFF_OUTPUT
constexpr const char* Output_suffix{".tiff"};
#elifdef JPEG_OUTPUT
constexpr const char* Output_suffix{".jpeg"};
#else
constexpr const char* Output_suffix{".png"};
#endif

/// @brief 渲染预计算颜色数据到 PNG，可选绘制色标
/// @param qcolor_fn 色标颜色映射函数，为空时不绘制色标
inline void rend_cl(const vector<cv::Vec3b>& comp, const vector<float>& lat, const vector<float>& lon,
                    string outname, const vector<string>& lbls, const string& ESlbl, int rows, int cols,
                    const std::function<cv::Vec3b(double, const RenderConfig&)>& qcolor_fn = nullptr,
                    const RenderConfig& cfg = {},
                    bool crop_area = false, double tlat = 0, double tlon = 0, double half = 7.5) {
    if (outname.empty())
    {
#ifndef OUTNAME_EQ_FILENAME
        outname = random_output_filename();
#else
        outname = lbls.front();
#endif
    }
    std::filesystem::create_directories(OUTPUT_DIR);
    string full_path = string(OUTPUT_DIR) + outname + Output_suffix;

    const vector<cv::Vec3b>* comp_view = &comp;
    const vector<float>* lat_view = &lat;
    const vector<float>* lon_view = &lon;
    vector<cv::Vec3b> comp_c;
    vector<float> lat_c, lon_c;
    double min_la = 0, max_la = 0, min_lo = 0, max_lo = 0;
    if (crop_area)
    {
        int r0 = 0, r1 = -1, c0 = 0, c1 = -1;
        if (!target_area_bounds(lat, lon, cols, tlat, tlon, half, r0, r1, c0, c1,
                                min_la, max_la, min_lo, max_lo))
            throw invalid_argument("目标区域不在数据覆盖范围内: " + to_string(tlat) + ", " + to_string(tlon));
        crop_flat(comp, lat, lon, cols, r0, r1, c0, c1, comp_c, lat_c, lon_c);
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++)
            {
                int si = r * cols + c, di = (r - r0) * (c1 - c0 + 1) + (c - c0);
                if (lat[si] < tlat - half || lat[si] > tlat + half ||
                    lon[si] < tlon - half || lon[si] > tlon + half)
                    comp_c[di] = {0, 0, 0};
            }
        comp_view = &comp_c;
        lat_view = &lat_c;
        lon_view = &lon_c;
        rows = r1 - r0 + 1;
        cols = c1 - c0 + 1;
    }
    if (!crop_area)
    {
        auto [min_it_la, max_it_la] = std::ranges::minmax_element(*lat_view);
        auto [min_it_lo, max_it_lo] = std::ranges::minmax_element(*lon_view);
        min_la = *min_it_la;
        max_la = *max_it_la;
        min_lo = *min_it_lo;
        max_lo = *max_it_lo;
    }
    const double la_range = max_la - min_la;
    const double lo_range = max_lo - min_lo;

    cout << "最小纬度 " << min_la << "; 最大纬度 " << max_la << '\n';
    cout << "最小经度 " << min_lo << "; 最大经度 " << max_lo << endl;

    const int offH = 324, offW = 300;
    const int W = max<int>(3840, rows * 1.6) + offW;
    const int H = static_cast<int>((W - offW) * (la_range / lo_range)) + offH;
    cv::Mat canvas(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
    const double x_factor = (W - offW - 1) / lo_range;
    const double y_factor = (H - offH - 1) / la_range;

    vector<cv::Point> top_vertices, bottom_vertices;
    compute_grid_vertex_row(0, rows, cols, *lat_view, *lon_view,
                            min_la, min_lo, x_factor, y_factor, H, offH, top_vertices);

    for (int r = 0; r < rows; ++r)
    {
        compute_grid_vertex_row(r + 1, rows, cols, *lat_view, *lon_view,
                                min_la, min_lo, x_factor, y_factor, H, offH, bottom_vertices);
        for (int c = 0; c < cols; ++c)
        {
            int idx = r * cols + c;
            cv::Point quad[4] = {
                top_vertices[c],
                top_vertices[c + 1],
                bottom_vertices[c + 1],
                bottom_vertices[c]
            };
            if (quad[0].x < 0 && quad[2].x < 0) continue;
            if (quad[0].x >= W && quad[2].x >= W) continue;
            if (quad[0].y < 0 && quad[2].y < 0) continue;
            if (quad[0].y >= H && quad[2].y >= H) continue;
            cv::fillConvexPoly(canvas, quad, 4, cv::Scalar((*comp_view)[idx]), cv::LINE_AA, 0);
        }
        top_vertices.swap(bottom_vertices);
    }

    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double font_scale = 1.5;
    const int thick = 2;
    const cv::Scalar front(255, 255, 255);

    cv::putText(canvas, to_string(min_lo), {0, H - 80}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, to_string(max_lo), {W - offW, H - 80}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, to_string(min_la), {W - offW, H - offH / 2}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, to_string(max_la), {W - offW, offH / 2}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, lbls[3] + " " + lbls[1] + " " +
                        lbls[2], {30, 64}, font, font_scale * 1.5, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, lbls[4], {20, H - 40}, font, font_scale, front, thick, cv::LINE_AA);
    auto sz_es = cv::getTextSize(ESlbl, font, font_scale, thick, nullptr);
    cv::putText(canvas, ESlbl, {W - sz_es.width - 20, H - 40}, font, font_scale * 1.2,
                front, thick, cv::LINE_AA);

    if (qcolor_fn)
    {
        const int b_w = 50, b_h = H;
        const int b_x = W - 50, b_y = (H - b_h) / 2;
        for (int i = 0; i < b_h; ++i)
        {
            float val = cfg.hival - (cfg.hival - cfg.lowval) * (static_cast<float>(i) / b_h);
            cv::line(canvas, {b_x, b_y + i}, {b_x + b_w, b_y + i}, cv::Scalar(qcolor_fn(val, cfg)));
        }
        cv::rectangle(canvas, {b_x, b_y}, {b_x + b_w, b_y + b_h}, front, 1);
        stringstream ss;
        ss << scientific << setprecision(2);
        ss.str("");
        ss << cfg.hival;
        cv::putText(canvas, ss.str(), {b_x - offW + 40, b_y + 50}, font, 1.5, front, 3);
        ss.str("");
        ss << cfg.lowval;
        cv::putText(canvas, ss.str(), {b_x - offW + 40, b_y + b_h - 40}, font, 1.5, front, 3);
    }

#ifdef  TIFF_OUTPUT
    cv::imwrite(full_path, canvas, {cv::IMWRITE_TIFF_COMPRESSION, 8});
#else
    cv::imwrite(full_path, canvas);
#endif
}

/// @brief 渲染预计算的 16 位颜色数据，可按经纬度区域裁剪
inline void rend_cl_16(const vector<cv::Vec<uint16_t, 3>>& comp,
                       const vector<float>& lat, const vector<float>& lon,
                       string outname, const vector<string>& lbls, const string& ESlbl,
                       int rows, int cols, bool crop_area = false, double tlat = 0,
                       double tlon = 0, double half = 7.5) {
    if (outname.empty())
    {
#ifndef OUTNAME_EQ_FILENAME
        outname = random_output_filename();
#else
        outname = lbls.front();
#endif
    }
    std::filesystem::create_directories(OUTPUT_DIR);
    string full_path = string(OUTPUT_DIR) + outname + ".tif";

    const vector<cv::Vec<uint16_t, 3>>* comp_view = &comp;
    const vector<float>* lat_view = &lat;
    const vector<float>* lon_view = &lon;
    vector<cv::Vec<uint16_t, 3>> comp_c;
    vector<float> lat_c, lon_c;
    double min_la = 0, max_la = 0, min_lo = 0, max_lo = 0;
    if (crop_area)
    {
        int r0 = 0, r1 = -1, c0 = 0, c1 = -1;
        if (!target_area_bounds(lat, lon, cols, tlat, tlon, half, r0, r1, c0, c1,
                                min_la, max_la, min_lo, max_lo))
            throw invalid_argument("目标区域不在数据覆盖范围内: " + to_string(tlat) + ", " + to_string(tlon));
        crop_flat(comp, lat, lon, cols, r0, r1, c0, c1, comp_c, lat_c, lon_c);
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++)
            {
                int si = r * cols + c, di = (r - r0) * (c1 - c0 + 1) + (c - c0);
                if (lat[si] < tlat - half || lat[si] > tlat + half ||
                    lon[si] < tlon - half || lon[si] > tlon + half)
                    comp_c[di] = {0, 0, 0};
            }
        comp_view = &comp_c;
        lat_view = &lat_c;
        lon_view = &lon_c;
        rows = r1 - r0 + 1;
        cols = c1 - c0 + 1;
    }
    if (!crop_area)
    {
        auto [min_it_la, max_it_la] = std::ranges::minmax_element(*lat_view);
        auto [min_it_lo, max_it_lo] = std::ranges::minmax_element(*lon_view);
        min_la = *min_it_la;
        max_la = *max_it_la;
        min_lo = *min_it_lo;
        max_lo = *max_it_lo;
    }
    const double la_range = max_la - min_la;
    const double lo_range = max_lo - min_lo;
    cout << "最小纬度 " << min_la << "; 最大纬度 " << max_la << '\n';
    cout << "最小经度 " << min_lo << "; 最大经度 " << max_lo << endl;

    const int offH = 324, offW = 100;
    const int W = max<int>(3840, rows * 1.6) + offW;
    const int H = static_cast<int>((W - offW) * (la_range / lo_range)) + offH;
    cv::Mat canvas(H, W, CV_16UC3, cv::Scalar(0, 0, 0));
    const double x_factor = (W - offW - 1) / lo_range;
    const double y_factor = (H - offH - 1) / la_range;

    vector<cv::Point> top_vertices, bottom_vertices;
    compute_grid_vertex_row(0, rows, cols, *lat_view, *lon_view,
                            min_la, min_lo, x_factor, y_factor, H, offH, top_vertices);
    for (int r = 0; r < rows; ++r)
    {
        compute_grid_vertex_row(r + 1, rows, cols, *lat_view, *lon_view,
                                min_la, min_lo, x_factor, y_factor, H, offH, bottom_vertices);
        for (int c = 0; c < cols; ++c)
        {
            int idx = r * cols + c;
            cv::Point quad[4] = {
                top_vertices[c],
                top_vertices[c + 1],
                bottom_vertices[c + 1],
                bottom_vertices[c]
            };
            if (quad[0].x < 0 && quad[2].x < 0) continue;
            if (quad[0].x >= W && quad[2].x >= W) continue;
            if (quad[0].y < 0 && quad[2].y < 0) continue;
            if (quad[0].y >= H && quad[2].y >= H) continue;
            const auto& color = (*comp_view)[idx];
            cv::fillConvexPoly(canvas, quad, 4,
                               cv::Scalar(color[0], color[1], color[2]), cv::LINE_AA, 0);
        }
        top_vertices.swap(bottom_vertices);
    }

    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double font_scale = 1.5;
    const int thick = 2;
    const cv::Scalar front(255, 255, 255);
    cv::Mat text8(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::putText(text8, to_string(min_lo), {0, H - 80}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(text8, to_string(max_lo), {W - offW, H - 80}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(text8, to_string(min_la), {W - offW, H - offH / 2}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(text8, to_string(max_la), {W - offW, offH / 2}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(text8, lbls[3] + " " + lbls[1] + " " + lbls[2],
                {30, 64}, font, font_scale * 1.5, front, thick, cv::LINE_AA);
    cv::putText(text8, lbls[4], {20, H - 40}, font, font_scale, front, thick, cv::LINE_AA);
    auto sz_es = cv::getTextSize(ESlbl, font, font_scale, thick, nullptr);
    cv::putText(text8, ESlbl, {W - sz_es.width - 20, H - 40}, font, font_scale * 1.2,
                front, thick, cv::LINE_AA);
    cv::Mat text16, text_mask;
    text8.convertTo(text16, CV_16UC3, 257.0);
    cv::cvtColor(text8, text_mask, cv::COLOR_BGR2GRAY);
    cv::threshold(text_mask, text_mask, 0, 255, cv::THRESH_BINARY);
    text16.copyTo(canvas, text_mask);
    cv::imwrite(full_path, canvas, {
                    cv::IMWRITE_TIFF_COMPRESSION, cv::IMWRITE_TIFF_COMPRESSION_ADOBE_DEFLATE
                });
}

/// @brief 渲染预计算颜色数据到 PNG，可选绘制色标
/// @param qcolor_fn 色标颜色映射函数，为空时不绘制色标
inline void rend_cl_matrix(const vector<cv::Vec3b>& comp,
                           string outname, const vector<string>& lbls, const string& ESlbl, int rows, int cols,
                           const std::function<cv::Vec3b(double, const RenderConfig&)>& qcolor_fn = nullptr,
                           const RenderConfig& cfg = {}) {
    if (outname.empty())
    {
#ifndef OUTNAME_EQ_FILENAME
        outname = random_output_filename();
#else
        outname = lbls.front();
#endif
    }
    std::filesystem::create_directories(OUTPUT_DIR);
    string full_path = string(OUTPUT_DIR) + outname + Output_suffix;

    cout << "绘制区域大小：" << rows << "*"
            << cols << ", " << comp.size() << " 像素" << endl;

    const int offH = 324, offW = 300;
    const int W = cols + offW;
    const int H = rows + offH;
    cv::Mat canvas(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat img(rows, cols, CV_8UC3, const_cast<cv::Vec3b*>(comp.data()));

    int x_offset = 0;
    int y_offset = (H - rows) / 2;

    cv::Rect roi(x_offset, y_offset, cols, rows);
    img.copyTo(canvas(roi));

    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double font_scale = 1.5;
    const int thick = 2;
    const cv::Scalar front(255, 255, 255);

    cv::putText(canvas, to_string(0), {0, H - 80}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, to_string(rows), {W - offW, H - 80}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, to_string(0), {W - offW, H - offH / 2}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, to_string(cols), {W - offW, offH / 2}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, lbls[3] + " " + lbls[1] + " " +
                        lbls[2], {30, 64}, font, font_scale * 1.5, front, thick,
                cv::LINE_AA);
    cv::putText(canvas, lbls[4], {20, H - 40}, font, font_scale, front, thick, cv::LINE_AA);
    auto sz_es = cv::getTextSize(ESlbl, font, font_scale, thick, nullptr);
    cv::putText(canvas, ESlbl, {W - sz_es.width - 20, H - 40}, font, font_scale * 1.2,
                front, thick, cv::LINE_AA);

    if (qcolor_fn)
    {
        const int b_w = 50, b_h = H;
        const int b_x = W - 50, b_y = (H - b_h) / 2;
        for (int i = 0; i < b_h; ++i)
        {
            float val = cfg.hival - (cfg.hival - cfg.lowval) * (static_cast<float>(i) / b_h);
            cv::line(canvas, {b_x, b_y + i}, {b_x + b_w, b_y + i}, cv::Scalar(qcolor_fn(val, cfg)));
        }
        cv::rectangle(canvas, {b_x, b_y}, {b_x + b_w, b_y + b_h}, front, 1);
        stringstream ss;
        ss << scientific << setprecision(2);
        ss.str("");
        ss << cfg.hival;
        cv::putText(canvas, ss.str(), {b_x - offW + 40, b_y + 50}, font, 1.5, front, 3);
        ss.str("");
        ss << cfg.lowval;
        cv::putText(canvas, ss.str(), {b_x - offW + 40, b_y + b_h - 40}, font, 1.5, front, 3);
    }

#ifdef  TIFF_OUTPUT
    cv::imwrite(full_path, canvas, {
                    cv::IMWRITE_TIFF_COMPRESSION, cv::IMWRITE_TIFF_COMPRESSION_ADOBE_DEFLATE
                });
#else
    cv::imwrite(full_path, canvas);
#endif
}

/// @brief 渲染预计算颜色数据到 TIFF（16-bit）
inline void rend_cl_matrix_16(const vector<cv::Vec<uint16_t, 3>>& comp,
                              string outname, const vector<string>& lbls, const string& ESlbl, int rows, int cols) {
    if (outname.empty())
    {
#ifndef OUTNAME_EQ_FILENAME
        outname = random_output_filename();
#else
        outname = lbls.front();
#endif
    }
    std::filesystem::create_directories(OUTPUT_DIR);
    string full_path = string(OUTPUT_DIR) + outname + ".tif";

    cout << "绘制区域大小：" << rows << "*"
            << cols << ", " << comp.size() << " 像素" << endl;

    const int offH = 324, offW = 100;
    const int W = cols + offW;
    const int H = rows + offH;

    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double font_scale = 1.5;
    const int thick = 2;
    const cv::Scalar front(255, 255, 255);

    cv::Mat canvas_text(H, W, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::putText(canvas_text, to_string(0), {0, H - 80}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas_text, to_string(rows), {W - offW, H - 80}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas_text, to_string(0), {W - offW, H - offH / 2}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas_text, to_string(cols), {W - offW, offH / 2}, font, font_scale * 0.8, front, thick,
                cv::LINE_AA);
    cv::putText(canvas_text, lbls[3] + " " + lbls[1] + " " +
                             lbls[2], {30, 64}, font, font_scale * 1.5, front, thick,
                cv::LINE_AA);
    cv::putText(canvas_text, lbls[4], {20, H - 40}, font, font_scale, front, thick, cv::LINE_AA);
    auto sz_es = cv::getTextSize(ESlbl, font, font_scale, thick, nullptr);
    cv::putText(canvas_text, ESlbl, {W - sz_es.width - 20, H - 40}, font, font_scale * 1.2,
                front, thick, cv::LINE_AA);

    cv::Mat canvas;
    canvas_text.convertTo(canvas, CV_16UC3, 257.0);
    cv::Mat img(rows, cols, CV_16UC3, const_cast<cv::Vec<uint16_t, 3>*>(comp.data()));

    int x_offset = 0;
    int y_offset = (H - rows) / 2;

    cv::Rect roi(x_offset, y_offset, cols, rows);
    img.copyTo(canvas(roi));
    cv::imwrite(full_path, canvas, {
                    cv::IMWRITE_TIFF_COMPRESSION, cv::IMWRITE_TIFF_COMPRESSION_ADOBE_DEFLATE
                });
}

/// @brief 渲染单波段数据到 PNG，内部将数据转为颜色后调用 rend_cl_matrix
template<typename QColorFn>
void rend_matrix(const vector<hsize_t>& dims, const vector<float>& mat, string outname,
                 const vector<string>& lbls, const string& ESlbl, QColorFn qcolor,
                 const RenderConfig& cfg) {
    vector<cv::Vec3b> comp(mat.size());
    for (size_t i = 0; i < mat.size(); i++)
        comp[i] = isnan(mat[i]) ? cv::Vec3b(0, 0, 0) : qcolor(mat[i], cfg);
    rend_cl_matrix(comp, std::move(outname), lbls, ESlbl, dims[0], dims[1], qcolor, cfg);
}

/// @brief 渲染单波段数据到 PNG，内部将数据转为颜色后调用 rend_cl
template<typename QColorFn>
void rend(const vector<hsize_t>& dims, const vector<float>& mat, const vector<float>& lat, const vector<float>& lon,
          string outname, const vector<string>& lbls, const string& ESlbl, QColorFn qcolor,
          const RenderConfig& cfg, bool crop_area = false, double tlat = 0, double tlon = 0,
          double half = 7.5) {
    vector<cv::Vec3b> comp(mat.size());
    for (size_t i = 0; i < mat.size(); i++)
        comp[i] = isnan(mat[i]) ? cv::Vec3b(0, 0, 0) : qcolor(mat[i], cfg);
    rend_cl(comp, lat, lon, std::move(outname), lbls, ESlbl, dims[1], dims[2], qcolor, cfg,
            crop_area, tlat, tlon, half);
}

#endif
