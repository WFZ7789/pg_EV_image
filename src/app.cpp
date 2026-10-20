/// SPDX-License-Identifier: MIT

#include "pipeline.hpp"
#include "native_swath.hpp"
#include "sensors/hdf.hpp"

#include <CLI/CLI.hpp>
#include <opencv2/core.hpp>

#include <iostream>
#include <stdexcept>

namespace {

std::string str(const cv::FileNode& node) {
    return node.empty() ? std::string() : static_cast<std::string>(node);
}

fy::Request load_job(const std::filesystem::path& filename) {
    cv::FileStorage file(filename.string(), cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON);
    if (!file.isOpened()) throw std::invalid_argument("无法打开任务文件: " + filename.string());
    fy::Request request;
    int version = int(file["version"]);
    if (version != 1) throw std::invalid_argument("任务配置 version 必须为 1");
    auto inputs = file["inputs"];
    if (!inputs.isSeq() || inputs.size() < 2) throw std::invalid_argument("拼接任务至少需要两组输入");
    for (const auto& item : inputs) {
        fy::Input input;
        auto resolve = [&](const std::string& value) {
            std::filesystem::path path(value);
            return path.is_absolute() ? path : filename.parent_path() / path;
        };
        auto data = str(item["data"]), geo = str(item["geo"]), aux = str(item["aux"]);
        if (data.empty()) throw std::invalid_argument("输入缺少 data 路径");
        input.data = resolve(data);
        if (!geo.empty()) input.geo = resolve(geo);
        if (!aux.empty()) input.aux = resolve(aux);
        request.inputs.push_back(input);
    }
    request.mode = str(file["mode"]);
    if (!file["band"].empty()) request.band = int(file["band"]);
    request.color = str(file["color"]);
    auto map = file["map"];
    if (!map.empty()) {
        if (!map["projection"].empty()) request.projection = str(map["projection"]);
        if (!map["width"].empty()) request.width = int(map["width"]);
        if (!map["center_lat"].empty()) request.center_lat = double(map["center_lat"]);
        if (!map["center_lon"].empty()) request.center_lon = double(map["center_lon"]);
        if (!map["half_range"].empty()) request.half_range = double(map["half_range"]);
        if (!map["grid"].empty()) request.grid = int(map["grid"]) != 0;
        if (!map["grid_step"].empty()) request.grid_step = double(map["grid_step"]);
    }
    auto output = str(file["output"]);
    if (!output.empty()) request.output = std::filesystem::path(output).is_absolute() ?
        std::filesystem::path(output) : filename.parent_path() / output;
    return request;
}

std::optional<std::string> adapter_for_mode(const std::string& mode) {
    if (mode == "LW" || mode == "MW" || mode == "IRall") return "giirs";
    if (mode == "S" || mode == "W" || mode == "W84" || mode == "TCX" || mode == "TCN" ||
        mode == "TCN2" || mode == "TCS" || mode == "TCR") return "mwri";
    if (mode == "PMR") return "pmr";
    if (mode == "Rr" || mode == "RTG" || mode == "RTpp" || mode == "RTEC" || mode == "RTEC16" ||
        mode == "RT16" || mode == "RT+EC" || mode == "AGHI_ALD" || mode == "RX") return "fy4";
    if (mode == "LL" || mode == "RT" || mode == "RMUL" || mode == "ECT" || mode == "EK" || mode == "TSG")
        return "mersi";
    return std::nullopt;
}

int execute(const fy::Request& request) {
    fy::core::ExecutionContext context(request.execution, "native");
    struct CvThreads {
        int previous = cv::getNumThreads();
        bool changed;
        explicit CvThreads(int threads) : changed(threads > 0) {
            if (changed) cv::setNumThreads(threads);
        }
        ~CvThreads() { if (changed) cv::setNumThreads(previous); }
    } cv_threads(request.execution.threads);
    auto write_profile = [&](bool success) {
        context.profile().set_scratch_bytes(context.scratch().bytes());
        context.profile().finish(success);
        if (request.execution.profile) {
            auto output = request.output.empty() ? std::filesystem::path("output/result.png") : request.output;
            output += ".profile.json";
            context.profile().write_json(output);
            std::cout << "性能报告 " << output << '\n';
        }
    };
    try {
        if (request.inputs.size() > 1) {
            context.profile().set_scope("native_mosaic");
            auto phase = context.profile().phase("native_render");
            fy::render_native_mosaic(request,context);
        }
        else {
            const auto& input = request.inputs.at(0);
            const auto hinted = adapter_for_mode(request.mode);
            const auto adapter = hinted ? *hinted : context.io().run([&] {
                return fy::sensors::identify_hdf_adapter({input.data,input.geo,input.aux});
            });
            if (adapter == "fy4") {
                context.profile().set_scope("native_disk");
                auto phase = context.profile().phase("native_render");
                fy::render_native_fy4(request,context);
            }
            else if (adapter == "giirs") {
                context.profile().set_scope("native_spectrum");
                auto phase = context.profile().phase("native_render");
                fy::render_native_giirs(request,context);
            }
            else {
                context.profile().set_scope(adapter == "mersi" || adapter == "mwri" || adapter == "pmr" ? "native_swath" : "native_scene");
                auto phase = context.profile().phase("native_render");
                fy::render_native_scene(request,adapter,context);
            }
        }
    } catch (...) {
        const auto original_error = std::current_exception();
        try { write_profile(false); }
        catch (const std::exception& error) { std::cerr << "性能报告失败: " << error.what() << '\n'; }
        std::rethrow_exception(original_error);
    }
    write_profile(true);
    return 0;
}

}

int main(int argc, char** argv) {
    CLI::App app{"风云卫星图像处理"};
    auto* inspect = app.add_subcommand("inspect", "查看输入元数据");
    auto* render = app.add_subcommand("render", "渲染单景产品");
    auto* mosaic = app.add_subcommand("mosaic", "拼接 FY-3 MERSI 连续轨段");
    std::string inspect_path, data, geo, aux, mode, color, output, projection = "geographic", job;
    int band = 0, width = 0;
    double center_lat = 0, center_lon = 0, half_range = 0, grid_step = 0;
    bool grid = false;
    fy::core::ExecutionOptions execution;
    inspect->add_option("data", inspect_path)->required();
    render->add_option("-d,--data", data)->required();
    render->add_option("-g,--geo", geo);
    render->add_option("-a,--aux", aux);
    render->add_option("-m,--mode", mode)->required();
    render->add_option("-b,--band", band);
    render->add_option("-c,--color", color);
    render->add_option("-o,--output", output);
    render->add_option("--projection", projection)->check(CLI::IsMember({"geographic","orthographic"}));
    render->add_option("--width", width);
    render->add_option("--center-lat", center_lat);
    render->add_option("--center-lon", center_lon);
    render->add_option("--half-range", half_range);
    render->add_option("--grid-step", grid_step);
    render->add_flag("--grid", grid);
    mosaic->add_option("--job", job)->required();
    for (auto* command : {render, mosaic}) {
        command->add_option("--threads", execution.threads, "计算线程数，0 为自动")
            ->check(CLI::NonNegativeNumber);
        command->add_flag("--profile", execution.profile, "保存性能 JSON 报告");
        command->add_option("--scratch-dir", execution.scratch_directory, "任务临时缓存的父目录");
    }
    CLI11_PARSE(app, argc, argv);
    try {
        if (inspect->parsed()) { fy::inspect(inspect_path); return 0; }
        if (mosaic->parsed()) {
            auto request = load_job(job);
            request.execution = execution;
            return execute(request);
        }
        if (!render->parsed()) { std::cout << app.help(); return 0; }
        fy::Request request;
        request.inputs.push_back({data,geo,aux});
        request.mode = mode; request.band = band; request.color = color;
        request.output = output; request.projection = projection; request.width = width;
        request.center_lat = center_lat; request.center_lon = center_lon;
        if (render->get_option("--half-range")->count()) request.half_range = half_range;
        request.grid_step = grid_step; request.grid = grid;
        request.execution = execution;
        return execute(request);
    } catch (const std::exception& error) {
        std::cerr << "处理失败: " << error.what() << '\n'; return 2;
    }
}
