/// SPDX-License-Identifier: MIT

#include "native_swath.hpp"
#include "render_product.hpp"
#include "products/execute.hpp"
#include "products/registry.hpp"
#include "render_detail.hpp"
#include "products/giirs.hpp"
#include "products/cache.hpp"
#include "products/statistics.hpp"
#include "products/phys.hpp"
#include "sensors/hdf.hpp"


#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <regex>

namespace fy {
namespace {

using namespace core;
using namespace render_detail;

std::chrono::sys_time<std::chrono::minutes> parse_time(const std::string& key) {
    const int year = std::stoi(key.substr(0,4)),hour = std::stoi(key.substr(8,2)),minute = std::stoi(key.substr(10,2));
    const auto month = static_cast<unsigned>(std::stoi(key.substr(4,2))),day = static_cast<unsigned>(std::stoi(key.substr(6,2)));
    const std::chrono::year_month_day date{std::chrono::year(year),std::chrono::month(month),std::chrono::day(day)};
    if (!date.ok() || hour > 23 || minute > 59) throw std::invalid_argument("Invalid mosaic observation time: " + key);
    return std::chrono::sys_days(date) + std::chrono::hours(hour) + std::chrono::minutes(minute);
}

}

void render_native_scene(const Request& request,const std::string& adapter,core::ExecutionContext& context) {
    AdapterRegistry sensor_registry;
    sensors::register_hdf_adapters(sensor_registry,context.profile());
    products::ProductRegistry product_registry;
    products::register_products(product_registry);
    render_registered_scene(request,adapter,sensor_registry,product_registry,context);
}

void render_registered_scene(const Request& request,const std::string& adapter,AdapterRegistry& sensor_registry,
                             products::ProductRegistry& product_registry,ExecutionContext& context) {
    validate_request(request);
    if (request.inputs.size() != 1) throw std::invalid_argument("Single-scene render requires one input");
    static_cast<void>(output_path(request));
    const auto& input = request.inputs.front();
    auto session = sensor_registry.open(adapter,{input.data,input.geo,input.aux});
    products::ProductRequest product_request;
    product_request.mode = request.mode; product_request.band = request.band; product_request.color = request.color;
    product_request.projection = {request.projection == "orthographic",request.center_lat,request.center_lon};
    const auto definition = product_registry.plan(adapter,session.metadata(),session.bands(),product_request);
    const auto region = definition.plan.regional_default_half_range ?
        std::optional(products::GeographicRegion{request.center_lat,request.center_lon,
            request.half_range.value_or(*definition.plan.regional_default_half_range)}) : std::nullopt;
    auto loaded = products::load_product(session,definition,context,region);
    render_product(request,loaded,context,{.bit_depth = definition.plan.frames.front().bit_depth,
                                           .region_already_selected = region.has_value()});
}

void render_native_giirs(const Request& request,ExecutionContext& context) {
    validate_request(request);
    if (request.inputs.size() != 1) throw std::invalid_argument("GIIRS render requires one input");
    if (request.projection != "geographic") throw std::invalid_argument("GIIRS 暂不支持正交投影");
    AdapterRegistry registry;
    sensors::register_hdf_adapters(registry,context.profile());
    const auto& input = request.inputs.front();
    auto session = registry.open("giirs",{input.data,input.geo,input.aux});
    std::optional<MapPlan> map;
    std::optional<ProjectedSwath> projected;
    std::optional<core::Raster<std::uint8_t>> crop;
    auto crop_memory = context.profile().reserve(BufferKind::scene,0);
    std::string previous_grid;
    const auto draw = [&](const products::PreparedProduct& product,const std::string& grid,const std::filesystem::path& path) {
        const auto& geometry = std::get<SwathGeometry>(*product.frame.geometry);
        const auto shape = geometry.latitude.shape();
        if (grid != previous_grid) {
            projected.reset(); crop.reset(); crop_memory.resize(0);
            map = plan_map(geometry,request,false);
            projected = project_swath(geometry,map->view,context);
            if (request.half_range) {
                crop_memory.resize(shape.elements());
                crop.emplace(shape);
            }
            previous_grid = grid;
        }
        SwathRasterSource source;
        source.cells = shape; source.vertices = projected->vertices.view(); source.display = product.display;
        source.source_columns = shape.cols;
        for (const auto& plane : product.frame.planes) {
            source.planes.push_back(plane.values.view()); source.valid.push_back(plane.valid.view());
        }
        if (crop) {
            for (std::size_t index = 0; index < shape.elements(); ++index)
                crop->values()[index] = product.frame.planes.front().valid.values()[index] && geometry.valid.values()[index] &&
                    std::abs(geometry.latitude.values()[index] - request.center_lat) <= *request.half_range &&
                    std::abs(std::remainder(geometry.longitude.values()[index] - request.center_lon,360.0)) <= *request.half_range;
            source.valid.front() = crop->view();
        }
        RasterOptions options;
        options.layout = map->layout; options.exact_coverage = false;
        auto rendered = rasterize_swaths(std::span(&source,1),options,context);
        save_frame(rendered,*map,product,request,path,context);
    };
    if (request.mode == "IRall") {
        const auto base = request.output.empty() ? std::filesystem::path("output/frame.jpg") : request.output;
        const auto directory = base.has_extension() ? base.parent_path() : base;
        const auto extension = base.has_extension() ? base.extension().string() : ".png";
        Request check = request;
        check.output = directory / ("000000" + extension);
        static_cast<void>(output_path(check));
        products::stream_giirs_sequence(session,context,[&](const auto& product,const auto& frame) {
            draw(product,frame.grid,directory / (frame.name + extension));
        });
    } else {
        const auto path = output_path(request);
        const auto definition = products::giirs_product(session.metadata(),session.bands(),
                                  {.mode = request.mode,.band = request.band,.color = request.color});
        auto loaded = products::load_product(session,definition,context);
        draw(loaded.product,definition.plan.calculation_grid,path);
    }
}

void render_native_fy4(const Request& request,ExecutionContext& context) {
    validate_request(request);
    if (request.inputs.size() != 1) throw std::invalid_argument("Disk render requires one input");
    if (request.projection != "geographic") throw std::invalid_argument("FY4 原生圆盘暂不支持正交投影");
    const auto path = output_path(request);
    AdapterRegistry registry;
    sensors::register_hdf_adapters(registry,context.profile());
    const auto& input = request.inputs.front();
    auto session = registry.open("fy4",{input.data,input.geo,input.aux});
    const auto metadata = session.metadata();
    products::ProductRequest product_request;
    product_request.mode = request.mode; product_request.band = request.band; product_request.color = request.color;
    products::ProductRegistry product_registry;
    products::register_products(product_registry);
    const auto definition = product_registry.plan("fy4",metadata,session.bands(),product_request);
    const auto bits = definition.plan.frames.front().bit_depth;
    if (bits == 16 && (path.extension() == ".jpg" || path.extension() == ".jpeg"))
        throw std::invalid_argument("16-bit output requires PNG or TIFF");
    auto loaded = products::load_product(session,definition,context);
    const auto shape = geometry_shape(*loaded.product.frame.geometry);
    if (request.half_range && (metadata.satellite != "FY-4B" ||
        !std::holds_alternative<NativeDiskGeometry>(*loaded.product.frame.geometry) || shape != metadata.grid("native").shape))
        throw std::invalid_argument("区域渲染目前仅支持具有完整导航的 FY-4B 全圆盘模式");
    render_product(request,loaded,context,{.bit_depth = bits});
}

void render_native_mosaic(const Request& request,ExecutionContext& context) {
    validate_request(request);
    if (request.inputs.size() < 2) throw std::invalid_argument("Mosaic requires at least two scenes");
    const auto path = output_path(request);
    AdapterRegistry registry;
    sensors::register_hdf_adapters(registry,context.profile());
    struct Entry {
        Input input;
        SensorMetadata metadata;
        std::optional<SensorSession> session;
        std::string time,resolution;
    };
    std::vector<Entry> entries;
    const std::regex timestamp(R"((20[0-9]{6})_([0-9]{4}))"),resolution(R"(_([0-9]{4}M)_)" );
    for (const auto& input : request.inputs) {
        if (input.geo.empty()) throw std::invalid_argument("Mosaic requires an explicit GEO pair for every scene");
        auto session = registry.open("mersi",{input.data,input.geo,input.aux});
        auto metadata = session.metadata();
        if (!metadata.satellite.starts_with("FY-3") || metadata.sensor.find("MERSI") == std::string::npos)
            throw std::invalid_argument("Mosaic accepts FY-3 MERSI scenes only");
        const auto name = input.data.filename().string();
        std::smatch time_match,resolution_match;
        if (!std::regex_search(name,time_match,timestamp) || !std::regex_search(name,resolution_match,resolution))
            throw std::invalid_argument("Mosaic filenames must contain date, time and resolution: " + name);
        const auto time = time_match[1].str() + time_match[2].str(),size = resolution_match[1].str();
        parse_time(time);
        if (!entries.empty() && (metadata.satellite != entries.front().metadata.satellite || metadata.sensor != entries.front().metadata.sensor ||
                                 metadata.resolution != entries.front().metadata.resolution || size != entries.front().resolution))
            throw std::invalid_argument("Mosaic scenes use different satellites, sensors or resolutions");
        entries.push_back({input,std::move(metadata),std::move(session),time,size});
    }
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b) { return std::tie(a.time,a.input.data) < std::tie(b.time,b.input.data); });
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].time == entries[i - 1].time) throw std::invalid_argument("Mosaic has duplicate observation times");
        const auto gap = parse_time(entries[i].time) - parse_time(entries[i - 1].time);
        if (gap > std::chrono::minutes(10)) std::cerr << "提示: 轨段时间间隔为 " << gap.count() << " 分钟，未覆盖区域将留空\n";
    }
    products::SharedDisplayStatistics statistics(request.mode,context.profile());
    std::vector<std::unique_ptr<products::CachedSwath>> caches;
    MapProjection projection{request.projection == "orthographic",request.center_lat,request.center_lon};
    MapBounds bounds{INFINITY,-INFINITY,INFINITY,-INFINITY};
    double low_longitude = INFINITY,high_longitude = -INFINITY,low_positive = INFINITY,high_positive = -INFINITY;
    std::uint64_t samples = 0;
    std::size_t native_rows = 0;
    for (auto& entry : entries) {
        auto phase = context.profile().phase("prepare_scene");
        products::ProductRequest product_request;
        product_request.mode = request.mode; product_request.band = request.band; product_request.color = request.color;
        product_request.projection = projection; product_request.mosaic = true;
        const auto definition = products::mersi_product(entry.metadata,entry.session->bands(),product_request);
        auto loaded = products::load_product(*entry.session,definition,context);
        const auto& geometry = std::get<SwathGeometry>(*loaded.product.frame.geometry);
        const auto shape = geometry.latitude.shape();
        if (shape.elements() > std::numeric_limits<std::uint32_t>::max() - samples) throw std::overflow_error("Mosaic exceeds compact source capacity");
        samples += shape.elements(); native_rows = std::max(native_rows,shape.rows);
        {
            auto statistics_phase = context.profile().phase("shared_statistics");
            statistics.add(loaded.product);
        }
        for (std::size_t i = 0; i < shape.elements(); ++i) {
            if (!geometry.valid.values()[i]) continue;
            const double longitude = std::remainder(geometry.longitude.values()[i],360.0),positive = longitude < 0 ? longitude + 360 : longitude;
            low_longitude = std::min(low_longitude,longitude); high_longitude = std::max(high_longitude,longitude);
            low_positive = std::min(low_positive,positive); high_positive = std::max(high_positive,positive);
            const auto point = projection.project(geometry.latitude.values()[i],geometry.longitude.values()[i]);
            if (!point.valid) continue;
            bounds.left = std::min(bounds.left,point.x); bounds.right = std::max(bounds.right,point.x);
            bounds.bottom = std::min(bounds.bottom,point.y); bounds.top = std::max(bounds.top,point.y);
        }
        caches.push_back(std::make_unique<products::CachedSwath>(context.scratch().path() / ("scene-" + std::to_string(caches.size())),loaded,context));
        entry.session.reset();
    }
    if (!std::isfinite(bounds.left)) throw std::invalid_argument("Mosaic has no visible valid geolocation");
    if (projection.orthographic) {
        const double radius = request.half_range ? std::sin(*request.half_range * Deg2rad) : 1;
        bounds = {-radius,radius,-radius,radius};
    } else if (request.half_range) {
        bounds = {request.center_lon - *request.half_range,request.center_lon + *request.half_range,
                  std::max(-90.0,request.center_lat - *request.half_range),std::min(90.0,request.center_lat + *request.half_range)};
    } else if (high_longitude - low_longitude <= 180) {
        bounds.left = low_longitude; bounds.right = high_longitude; projection.center_longitude = (low_longitude + high_longitude) / 2;
    } else {
        bounds.left = low_positive; bounds.right = high_positive; projection.center_longitude = 180;
    }
    if (bounds.left >= bounds.right || bounds.bottom >= bounds.top || (!projection.orthographic && bounds.right - bounds.left > 180))
        throw std::invalid_argument("Invalid mosaic map extent; specify a center and half range");
    const auto baseline = std::max<std::size_t>(3840,static_cast<std::size_t>(native_rows * 1.6));
    const auto width = request.width ? static_cast<std::size_t>(request.width) : baseline;
    const double raw_height = projection.orthographic ? width : width * (bounds.top - bounds.bottom) / (bounds.right - bounds.left);
    if (!std::isfinite(raw_height) || raw_height > 32000 || width > 32000) throw std::invalid_argument("Mosaic dimensions exceed 32000 pixels");
    const Shape pixels{std::max<std::size_t>(1,static_cast<std::size_t>(raw_height)),width};
    if (pixels.elements() > 600000000) throw std::invalid_argument("Mosaic exceeds 600 million pixels");
    const MapPlan map{{projection,bounds,pixels},LayoutTransform::make(pixels,{.baseline_map_width = double(baseline)})};
    const auto scale = statistics.scale();
    if (scale) context.profile().set_shared_display_scale({scale->low,scale->high});
    std::vector<SwathRasterSource> sources;
    for (std::size_t i = 0; i < caches.size(); ++i) {
        if (scale) caches[i]->display().scale = *scale;
        caches[i]->project(map.view); sources.push_back(caches[i]->source(i));
    }
    context.profile().set_source_pixels(samples);
    products::PreparedProduct legend;
    legend.display = caches.front()->display(); legend.labels = caches.front()->labels(); legend.extra_label = caches.front()->extra_label();
    RasterOptions options;
    options.layout = map.layout; options.overlap = OverlapPolicy::scan_center; options.exact_coverage = false;
    auto rendered = rasterize_swaths(sources,options,context);
    save_frame(rendered,map,legend,request,path,context);
}

}
