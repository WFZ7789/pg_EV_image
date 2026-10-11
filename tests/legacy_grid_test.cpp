/// SPDX-License-Identifier: MIT

#include "legacy/rend.h"
#include <chrono>

int main() {
    auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    struct TemporaryDirectory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("fy3-legacy-grid-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    } temporary;
    const int rows = 4, cols = 6;
    std::vector<float> lat(rows * cols), lon(rows * cols);
    std::vector<cv::Vec3b> colors(rows * cols, {80, 100, 120});
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            lat[r * cols + c] = 10.f + r / 3.f;
            lon[r * cols + c] = 128.f + c * .8f;
        }
    std::vector<std::string> labels{"scene.HDF", "2026-07-30", "02:20:00(UTC)", "FY-3H MERSI",
                                    "250m Reflective Bands Earth View Data Band 1"};
    auto mapper = [](double value, const RenderConfig&) {
        auto gray = static_cast<unsigned char>(std::clamp(value * 100, 0., 255.));
        return cv::Vec3b(gray, gray, gray);
    };
    auto render = [&](const char* name, const LegacyGrid& grid, int width = 0) {
        auto path = temporary.path / name;
        rend_cl(colors, lat, lon, path.string(), labels, "", rows, cols, mapper, {0, 2},
                false, 0, 0, 7.5, grid, width);
        auto image = cv::imread(path.string());
        require(!image.empty(), "旧版成图写入失败");
        return image;
    };
    auto original = render("default.png", {});
    require(original.cols == 4140 && original.rows == 1284, "旧版画布尺寸发生变化");
    auto disabled = render("disabled.png", {false, .25, true, 15, 130});
    require(cv::norm(original, disabled, cv::NORM_INF) == 0, "关闭 grid 后仍改变了图像");
    auto check_grid = [&](const cv::Mat& reference, const LegacyGrid& grid) {
        auto enabled = render("enabled.png", grid);
        require(enabled.size() == reference.size(), "grid 改变了画布尺寸");
        cv::Mat difference;
        cv::absdiff(reference, enabled, difference);
        require(cv::countNonZero(difference.reshape(1)) > 0, "grid 未绘制");
        difference(cv::Rect(0, 162, 3840, reference.rows - 324)).setTo(cv::Scalar(0, 0, 0));
        require(cv::countNonZero(difference.reshape(1)) == 0, "grid 改动了标题、页脚或色条区域");
    };
    check_grid(original, {true, .25});
    check_grid(original, {true, 0});
    auto resized = render("resized.png", {}, 960);
    require(resized.cols == 1035 && resized.rows == 321, "显式宽度未等比缩放完整旧版画布");
    cv::Mat expected;
    cv::resize(original, expected, resized.size(), 0, 0, cv::INTER_AREA);
    require(cv::norm(resized, expected, cv::NORM_INF) == 0, "缩放时改变了旧版布局");
    for (size_t i = 0; i < lat.size(); ++i) {
        auto point = legacy_orthographic(lat[i], lon[i], 10.5, 130);
        lat[i] = point.first;
        lon[i] = point.second;
    }
    auto orthographic = render("orthographic.png", {});
    check_grid(orthographic, {true, .25, true, 10.5, 130});
    check_grid(orthographic, {true, 0, true, 10.5, 130});
}
