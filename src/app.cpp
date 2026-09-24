#include "pipeline.hpp"
#include "config.h"
#include "read.h"

#include <CLI/CLI.hpp>
#include <H5Cpp.h>
#include <opencv2/core.hpp>

#include <iostream>
#include <stdexcept>

int legacy_main(int argc, char** argv);

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

int run_legacy(const fy::Request& request) {
    const auto& input = request.inputs.front();
    std::vector<std::string> args{"pg_EV_image", "-m", request.mode, "-d", input.data.string()};
    if (!input.geo.empty()) { args.push_back("-g"); args.push_back(input.geo.string()); }
    if (!input.aux.empty()) { args.push_back("-a"); args.push_back(input.aux.string()); }
    args.push_back("-o");
    args.push_back(request.output.empty() ? "output/result.png" : request.output.string());
    args.push_back("-b"); args.push_back(std::to_string(request.band));
    if (!request.color.empty()) { args.push_back("-c"); args.push_back(request.color); }
    if (request.half_range) {
        args.push_back("--crop"); args.push_back("-r"); args.push_back(std::to_string(*request.half_range));
        args.push_back("-x"); args.push_back(std::to_string(request.center_lat));
        args.push_back("-y"); args.push_back(std::to_string(request.center_lon));
    }
    if (request.projection == "orthographic") args.push_back("--aq");
    std::vector<char*> raw;
    for (auto& arg : args) raw.push_back(arg.data());
    return legacy_main(int(raw.size()), raw.data());
}

}

int main(int argc, char** argv) {
    CLI::App app{"风云卫星图像处理"};
    auto* inspect = app.add_subcommand("inspect", "查看输入元数据");
    auto* render = app.add_subcommand("render", "渲染单景产品");
    auto* mosaic = app.add_subcommand("mosaic", "拼接 FY-3 MERSI 连续轨段");
    std::string inspect_path, data, geo, aux, mode, color, output, projection = "geographic", job;
    int band = 0, width = 3840;
    double center_lat = 0, center_lon = 0, half_range = 0, grid_step = 0;
    bool no_grid = false;
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
    render->add_flag("--no-grid", no_grid);
    mosaic->add_option("--job", job)->required();
    CLI11_PARSE(app, argc, argv);
    try {
        if (inspect->parsed()) { fy::inspect(inspect_path); return 0; }
        if (mosaic->parsed()) { fy::render(load_job(job)); return 0; }
        if (!render->parsed()) { std::cout << app.help(); return 0; }
        fy::Request request;
        request.inputs.push_back({data,geo,aux});
        request.mode = mode; request.band = band; request.color = color;
        request.output = output; request.projection = projection; request.width = width;
        request.center_lat = center_lat; request.center_lon = center_lon;
        if (render->get_option("--half-range")->count()) request.half_range = half_range;
        request.grid_step = grid_step; request.grid = !no_grid;
        H5::H5File source(data, H5F_ACC_RDONLY);
        std::string sat = read_str_attr(source.openGroup("/").openAttribute("Satellite Name"));
        std::string sensor = read_str_attr(source.openAttribute("Sensor Identification Code"));
        if (sat.rfind("FY-3",0) == 0 && sensor.find("MERSI") != std::string::npos) {
            fy::render(request); return 0;
        }
        return run_legacy(request);
    } catch (const H5::Exception& error) {
        std::cerr << "HDF5 错误: " << error.getDetailMsg() << '\n'; return 2;
    } catch (const std::exception& error) {
        std::cerr << "处理失败: " << error.what() << '\n'; return 2;
    }
}
