/// SPDX-License-Identifier: MIT

#include "render_product.hpp"
#include "render_detail.hpp"
#include "core/navigation.hpp"
#include "products/phys.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fy::render_detail {

using namespace core;

void validate_request(const Request& request,bool require_input) {
    if (require_input && (request.inputs.empty() || request.mode.empty())) throw std::invalid_argument("Missing native render input or mode");
    if (request.width && (request.width < 256 || request.width > 32000)) throw std::invalid_argument("Map width must be 0 or 256 to 32000");
    if (request.projection != "geographic" && request.projection != "orthographic") throw std::invalid_argument("Unknown map projection");
    if (!std::isfinite(request.center_lat) || std::abs(request.center_lat) > 90 || !std::isfinite(request.center_lon))
        throw std::invalid_argument("Invalid map center");
    if (request.half_range && (!std::isfinite(*request.half_range) || *request.half_range <= 0 || *request.half_range > 90))
        throw std::invalid_argument("Invalid map half range");
    if (!std::isfinite(request.grid_step) || request.grid_step < 0 || request.grid_step > 90)
        throw std::invalid_argument("Invalid grid step");
}

std::filesystem::path output_path(const Request& request) {
    auto path = request.output.empty() ? std::filesystem::path("output/result.png") : request.output;
    if (path.extension().empty()) path += ".png";
    const auto extension = path.extension().string();
    if (extension != ".png" && extension != ".jpg" && extension != ".jpeg" && extension != ".tif" && extension != ".tiff")
        throw std::invalid_argument("Unsupported output format: " + extension);
    return path;
}

MapPlan plan_map(const SwathGeometry& geometry,const Request& request,bool cropped) {
    MapProjection projection{request.projection == "orthographic",request.center_lat,request.center_lon};
    if (!projection.orthographic && !request.half_range) {
        double low = INFINITY,high = -INFINITY;
        for (std::size_t i = 0; i < geometry.valid.values().size(); ++i)
            if (geometry.valid.values()[i]) {
                const double value = std::remainder(geometry.longitude.values()[i],360.0);
                low = std::min(low,value); high = std::max(high,value);
            }
        if (!std::isfinite(low)) throw std::invalid_argument("No valid geolocation samples");
        projection.center_longitude = high - low <= 180 ? (low + high) / 2 : 180;
    }
    MapBounds bounds{INFINITY,-INFINITY,INFINITY,-INFINITY};
    const auto shape = geometry.latitude.shape();
    std::size_t first_row = shape.rows,last_row = 0;
    for (std::size_t i = 0; i < shape.elements(); ++i) {
        if (!geometry.valid.values()[i]) continue;
        const auto point = projection.project(geometry.latitude.values()[i],geometry.longitude.values()[i]);
        if (!point.valid) continue;
        if (request.half_range && !cropped) {
            const double half = projection.orthographic ? std::sin(*request.half_range * Deg2rad) : *request.half_range;
            const double x = projection.orthographic ? 0 : request.center_lon, y = projection.orthographic ? 0 : request.center_lat;
            if (std::abs(point.x - x) > half || std::abs(point.y - y) > half) continue;
        }
        bounds.left = std::min(bounds.left,point.x); bounds.right = std::max(bounds.right,point.x);
        bounds.bottom = std::min(bounds.bottom,point.y); bounds.top = std::max(bounds.top,point.y);
        first_row = std::min(first_row,i / shape.cols); last_row = std::max(last_row,i / shape.cols);
    }
    if (!std::isfinite(bounds.left) || bounds.left >= bounds.right || bounds.bottom >= bounds.top ||
        (!projection.orthographic && bounds.right - bounds.left > 180))
        throw std::invalid_argument("Map region has no valid extent; specify a center and half range");
    const auto baseline = std::max<std::size_t>(3840,static_cast<std::size_t>((last_row - first_row + 1) * 1.6));
    const auto width = request.width ? static_cast<std::size_t>(request.width) : baseline;
    const double raw_height = width * (bounds.top - bounds.bottom) / (bounds.right - bounds.left);
    if (!std::isfinite(raw_height) || raw_height > 16000 || width > 16000)
        throw std::invalid_argument("Map dimensions exceed 16000 pixels");
    const Shape pixels{std::max<std::size_t>(1,static_cast<std::size_t>(raw_height)),width};
    if (pixels.elements() > 256000000) throw std::invalid_argument("Map exceeds 256 million pixels");
    return {{projection,bounds,pixels},LayoutTransform::make(pixels,{.baseline_map_width = double(baseline)})};
}

void annotations(cv::Mat& canvas,const MapView& view,const LayoutTransform& layout,const products::PreparedProduct& product,
                  bool image_axes) {
    const auto scale = layout.scale;
    const auto x = [scale](double value) { return cvRound(value * scale); };
    const int width = canvas.cols,height = canvas.rows,margin = width - int(layout.left + layout.map.cols),thickness = std::max(1,x(2));
    constexpr auto font = cv::FONT_HERSHEY_SIMPLEX;
    const cv::Scalar white(255,255,255);
    auto text = [&](const std::string& value,cv::Point anchor,double font_size,int weight = 0) {
        cv::putText(canvas,value,anchor,font,std::max(0.05,font_size * scale),white,weight ? weight : thickness,cv::LINE_AA);
    };
    const double unit = view.projection.orthographic ? 1 / Deg2rad : 1;
    const auto coordinate = [image_axes,unit](double value) { return image_axes ? std::to_string(std::llround(value)) : std::to_string(value * unit); };
    text(coordinate(view.bounds.left),{0,height - x(80)},1.2);
    text(coordinate(view.bounds.right),{width - margin,height - x(80)},1.2);
    text(coordinate(view.bounds.bottom),{width - margin,height - x(162)},1.2);
    text(coordinate(view.bounds.top),{width - margin,x(162)},1.2);
    const auto& labels = product.labels;
    text(labels.at(3) + " " + labels.at(1) + " " + labels.at(2),{x(30),x(64)},2.25);
    text(labels.at(4),{x(20),height - x(40)},1.5);
    const auto extra_size = cv::getTextSize(product.extra_label,font,1.5 * scale,thickness,nullptr);
    text(product.extra_label,{width - extra_size.width - x(20),height - x(40)},1.8);
    if (!product.display.colorbar) return;
    const auto& display = product.display;
    const int bar_width = std::max(1,x(50)),bar_x = width - bar_width;
    for (int row = 0; row < height; ++row) {
        const float low = display.scale.low,high = display.scale.high;
        const float value = high - (high - low) * (float(row) / height);
        const auto color = display.mapper(std::span(&value,1),display.scale);
        const cv::Scalar bgr(std::clamp(color.blue,0.0,1.0) * 255,std::clamp(color.green,0.0,1.0) * 255,
                             std::clamp(color.red,0.0,1.0) * 255);
        cv::line(canvas,{bar_x,row},{width - 1,row},bgr);
    }
    cv::rectangle(canvas,{bar_x,0},{width - 1,height - 1},white,std::max(1,x(1)));
    std::ostringstream label;
    label << std::scientific << std::setprecision(2) << float(display.scale.high);
    text(label.str(),{bar_x - margin + x(40),x(50)},1.5,std::max(1,x(3)));
    label.str(""); label << float(display.scale.low);
    text(label.str(),{bar_x - margin + x(40),height - x(40)},1.5,std::max(1,x(3)));
}

void grid(cv::Mat& canvas,const MapView& view,const LayoutTransform& layout,const Request& request,ProfileReport& profile,
           const NativeDiskGeometry* disk) {
    if (!request.grid) return;
    const auto& bounds = view.bounds;
    const auto& projection = view.projection;
    const double unit = projection.orthographic ? 1 / Deg2rad : 1;
    double step = request.grid_step;
    if (step <= 0) {
        step = 60;
        for (double candidate : {1.,2.,5.,10.,15.,20.,30.,45.,60.})
            if (candidate >= (disk ? 180 : std::max(bounds.top - bounds.bottom,bounds.right - bounds.left) * unit) / 8) {
                step = candidate; break;
            }
    }
    const cv::Rect region(static_cast<int>(layout.left),static_cast<int>(layout.top),
                          static_cast<int>(layout.map.cols),static_cast<int>(layout.map.rows));
    const bool wide = canvas.depth() == CV_16U;
    auto memory = profile.reserve(BufferKind::framebuffer,layout.map.elements() * (wide ? 4 : 3));
    auto target = canvas(region);
    auto layer = wide ? cv::Mat(region.height,region.width,CV_8UC3,cv::Scalar{}) : target.clone();
    cv::Mat opacity;
    if (wide) opacity = cv::Mat(region.height,region.width,CV_8UC1,cv::Scalar{});
    const double left = disk ? disk->longitude_deg - 90 : projection.orthographic ? projection.center_longitude - 180 : bounds.left;
    const double right = disk ? disk->longitude_deg + 90 : projection.orthographic ? projection.center_longitude + 180 : bounds.right;
    const double bottom = disk || projection.orthographic ? -90 : bounds.bottom,top = disk || projection.orthographic ? 90 : bounds.top;
    if ((right - left + top - bottom) / step > 4096)
        throw std::invalid_argument("Grid spacing produces more than 4096 lines");
    auto trace = [&](bool meridian,double fixed) {
        const double begin = meridian ? bottom : left,end = meridian ? top : right;
        const int count = std::max(100,static_cast<int>(std::max(view.pixels.rows,view.pixels.cols) * 4));
        bool connected = false,labelled = false;
        cv::Point previous;
        for (int i = 0; i <= count; ++i) {
            const double varying = begin + (end - begin) * i / count;
            double px,py;
            if (disk) {
                const auto point = disk_image_at(*disk,meridian ? varying : fixed,meridian ? fixed : varying);
                if (!point.valid) { connected = false; continue; }
                px = (point.col + 0.5) * region.width / disk->shape.cols;
                py = (point.row + 0.5) * region.height / disk->shape.rows;
            } else {
                const auto projected = projection.project(meridian ? varying : fixed,meridian ? fixed : varying);
                if (!projected.valid) { connected = false; continue; }
                px = (projected.x - bounds.left) * (region.width - 1) / (bounds.right - bounds.left);
                py = (bounds.top - projected.y) * (region.height - 1) / (bounds.top - bounds.bottom);
            }
            if (px < -1 || py < -1 || px > region.width || py > region.height) { connected = false; continue; }
            const cv::Point point(cvRound(px),cvRound(py));
            const bool in = cv::Rect(0,0,region.width,region.height).contains(point);
            if (in && connected && cv::norm(point - previous) < std::max(2.0,25 * layout.scale)) {
                cv::line(layer,previous,point,{220,220,220},1,cv::LINE_AA);
                if (wide) cv::line(opacity,previous,point,255,1,cv::LINE_AA);
            }
            if (in && !labelled) {
                const double value = meridian ? std::remainder(fixed,360.) : fixed;
                std::ostringstream label;
                label << std::abs(value) << (meridian ? (value < 0 ? "W" : "E") : (value < 0 ? "S" : "N"));
                const double font_scale = std::max(0.05,0.9 * layout.scale);
                int baseline = 0;
                const auto size = cv::getTextSize(label.str(),cv::FONT_HERSHEY_SIMPLEX,font_scale,1,&baseline);
                cv::Point position(std::clamp(point.x + cvRound(8 * layout.scale),2,std::max(2,region.width - size.width - 2)),
                    std::clamp(point.y + size.height + cvRound(8 * layout.scale),size.height + 2,
                               std::max(size.height + 2,region.height - baseline - 2)));
                cv::putText(layer,label.str(),position,cv::FONT_HERSHEY_SIMPLEX,font_scale,{255,255,255},1,cv::LINE_AA);
                if (wide) cv::putText(opacity,label.str(),position,cv::FONT_HERSHEY_SIMPLEX,font_scale,255,1,cv::LINE_AA);
                labelled = true;
            }
            previous = point; connected = in;
        }
    };
    for (double longitude = std::ceil(left / step) * step; longitude <= right; longitude += step) trace(true,longitude);
    for (double latitude = std::ceil(bottom / step) * step; latitude <= top; latitude += step) trace(false,latitude);
    if (!wide) cv::addWeighted(layer,.55,target,.45,0,target);
    else for (int row = 0; row < region.height; ++row) {
        const auto* colors = layer.ptr<cv::Vec3b>(row);
        const auto* alpha = opacity.ptr<std::uint8_t>(row);
        auto* pixels = target.ptr<cv::Vec<std::uint16_t,3>>(row);
        for (int col = 0; col < region.width; ++col)
            if (alpha[col]) for (int channel = 0; channel < 3; ++channel)
                pixels[col][channel] = cv::saturate_cast<std::uint16_t>(pixels[col][channel] * (1 - .55 * alpha[col] / 255) +
                                                                     colors[col][channel] * 257 * .55);
    }
}

void save_frame(RasterizedFrame& rendered,const MapPlan& map,const products::PreparedProduct& product,
                const Request& request,const std::filesystem::path& path,ExecutionContext& context) {
    static_assert(sizeof(Pixel8) == 3);
    static_assert(sizeof(Pixel16) == 6);
    cv::Mat canvas = std::visit([](auto& pixels) {
        using Pixel = std::remove_reference_t<decltype(pixels.view().at(0,0))>;
        return cv::Mat(int(pixels.shape().rows),int(pixels.shape().cols),std::is_same_v<Pixel,Pixel8> ? CV_8UC3 : CV_16UC3,pixels.values().data());
    },rendered.pixels);
    {
        auto phase = context.profile().phase("annotations");
        grid(canvas,map.view,map.layout,request,context.profile(),map.disk ? &*map.disk : nullptr);
        if (canvas.depth() == CV_8U) annotations(canvas,map.view,map.layout,product,map.image_axes);
        else {
            auto memory = context.profile().reserve(BufferKind::framebuffer,map.layout.framebuffer.elements() * 3);
            cv::Mat overlay(canvas.rows,canvas.cols,CV_8UC3,cv::Scalar{});
            annotations(overlay,map.view,map.layout,product,map.image_axes);
            context.parallel_rows(std::size_t(canvas.rows),[&](std::size_t begin,std::size_t end) {
                for (auto row = begin; row < end; ++row) {
                    const auto* source = overlay.ptr<cv::Vec3b>(int(row));
                    auto* target = canvas.ptr<cv::Vec<std::uint16_t,3>>(int(row));
                    for (int col = 0; col < canvas.cols; ++col)
                        if (source[col] != cv::Vec3b{})
                            for (int channel = 0; channel < 3; ++channel) target[col][channel] = std::uint16_t(source[col][channel]) * 257;
                }
            });
        }
    }
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    {
        auto phase = context.profile().phase("encode");
        const bool tiff16 = canvas.depth() == CV_16U && (path.extension() == ".tif" || path.extension() == ".tiff");
        const auto parameters = tiff16 ? std::vector<int>{cv::IMWRITE_TIFF_COMPRESSION,cv::IMWRITE_TIFF_COMPRESSION_ADOBE_DEFLATE} :
                                        std::vector<int>{};
        if (!cv::imwrite(path.string(),canvas,parameters)) throw std::runtime_error("Image encoding failed: " + path.string());
    }
    std::cout << "已保存 " << path << " (" << canvas.cols << " x " << canvas.rows << ")\n";
}

DiskCrop plan_disk_crop(const NativeDiskGeometry& geometry,const Request& request,ExecutionContext& context) {
    MapProjection projection{false,request.center_lat,request.center_lon};
    MapBounds bounds{INFINITY,-INFINITY,INFINITY,-INFINITY};
    std::size_t first_row = geometry.shape.rows,last_row = 0,first_col = geometry.shape.cols,last_col = 0;
    struct RowBounds { std::size_t first,last; MapBounds bounds; };
    auto memory = context.profile().reserve(BufferKind::statistics,geometry.shape.rows * sizeof(RowBounds));
    std::vector<RowBounds> rows(geometry.shape.rows);
    context.parallel_rows(geometry.shape.rows,[&](std::size_t begin,std::size_t end) {
        for (auto row = begin; row < end; ++row) {
            RowBounds result{geometry.shape.cols,0,{INFINITY,-INFINITY,INFINITY,-INFINITY}};
            for (std::size_t col = 0; col < geometry.shape.cols; ++col) {
                const auto location = disk_geographic_at(geometry,row,col);
                if (!location.valid) continue;
                const auto point = projection.project(float(location.latitude),float(location.longitude));
                if (std::abs(point.x - request.center_lon) > *request.half_range ||
                    std::abs(point.y - request.center_lat) > *request.half_range) continue;
                result.first = std::min(result.first,col); result.last = std::max(result.last,col);
                result.bounds.left = std::min(result.bounds.left,point.x); result.bounds.right = std::max(result.bounds.right,point.x);
                result.bounds.bottom = std::min(result.bounds.bottom,point.y); result.bounds.top = std::max(result.bounds.top,point.y);
            }
            rows[row] = result;
        }
    });
    for (std::size_t row = 0; row < rows.size(); ++row) {
        const auto& result = rows[row];
        if (result.first == geometry.shape.cols) continue;
        first_row = std::min(first_row,row); last_row = std::max(last_row,row);
        first_col = std::min(first_col,result.first); last_col = std::max(last_col,result.last);
        bounds.left = std::min(bounds.left,result.bounds.left); bounds.right = std::max(bounds.right,result.bounds.right);
        bounds.bottom = std::min(bounds.bottom,result.bounds.bottom); bounds.top = std::max(bounds.top,result.bounds.top);
    }
    if (first_row == geometry.shape.rows || bounds.right <= bounds.left || bounds.top <= bounds.bottom)
        throw std::invalid_argument("Requested region is outside the disk coverage");
    const auto baseline = std::max<std::size_t>(3840,std::size_t((last_row - first_row + 1) * 1.6));
    const auto width = request.width ? std::size_t(request.width) : baseline;
    const auto raw_height = width * (bounds.top - bounds.bottom) / (bounds.right - bounds.left);
    if (!std::isfinite(raw_height) || width > 16000 || raw_height > 16000)
        throw std::invalid_argument("Disk crop dimensions exceed supported limits");
    const auto height = std::max<std::size_t>(1,std::size_t(raw_height));
    const Shape pixels{height,width};
    return {{{projection,bounds,pixels},LayoutTransform::make(pixels,{.baseline_map_width = double(baseline)})},
             {first_row,first_col,{last_row - first_row + 1,last_col - first_col + 1}}};
}

}

namespace fy {

using namespace core;
using namespace render_detail;

void render_product(const Request& request,const products::LoadedProduct& loaded,ExecutionContext& context,
                    const ProductRenderOptions& options) {
    validate_request(request,false);
    if (options.bit_depth != 8 && options.bit_depth != 16) throw std::invalid_argument("Unsupported output bit depth");
    const auto path = output_path(request);
    if (options.bit_depth == 16 && (path.extension() == ".jpg" || path.extension() == ".jpeg"))
        throw std::invalid_argument("16-bit output requires PNG or TIFF");
    loaded.product.frame.validate();
    if (std::holds_alternative<SwathGeometry>(*loaded.product.frame.geometry)) {
        const auto& product = loaded.product;
        const auto& geometry = std::get<SwathGeometry>(*product.frame.geometry);
        const auto map = plan_map(geometry,request,options.region_already_selected);
        auto projected = project_swath(geometry,map.view,context);
        SwathRasterSource source;
        source.cells = geometry.latitude.shape(); source.vertices = projected.vertices.view(); source.display = product.display;
        source.source_column = loaded.source_window.col; source.source_columns = loaded.source_shape.cols;
        for (const auto& plane : product.frame.planes) {
            source.planes.push_back(plane.values.view()); source.valid.push_back(plane.valid.view());
        }
        RasterOptions raster_options;
        raster_options.layout = map.layout; raster_options.bit_depth = options.bit_depth;
        raster_options.exact_coverage = false;
        auto rendered = rasterize_swaths(std::span(&source,1),raster_options,context);
        save_frame(rendered,map,product,request,path,context);
        return;
    }
    if (!std::holds_alternative<NativeDiskGeometry>(*loaded.product.frame.geometry) &&
        !std::holds_alternative<ImageGeometry>(*loaded.product.frame.geometry))
        throw std::invalid_argument("Unsupported rendering geometry");
    if (request.projection != "geographic") throw std::invalid_argument("Native image requires geographic axes");
    const auto bits = options.bit_depth;
    const auto& product = loaded.product;
    const auto shape = geometry_shape(*product.frame.geometry);
    const auto* disk = std::get_if<NativeDiskGeometry>(product.frame.geometry.get());
    if (request.half_range) {
        if (!disk) throw std::invalid_argument("Geographic crop requires disk navigation");
        const auto crop = [&] { auto phase = context.profile().phase("map_planning"); return plan_disk_crop(*disk,request,context); }();
        auto cropped_disk = *disk;
        cropped_disk.shape = crop.window.shape;
        cropped_disk.scan_origin_rad[0] += crop.window.col * disk->scan_step_rad[0];
        cropped_disk.scan_origin_rad[1] += crop.window.row * disk->scan_step_rad[1];
        auto projected = project_disk(cropped_disk,crop.map.view,context);
        SwathRasterSource source;
        source.cells = crop.window.shape; source.vertices = projected.vertices.view(); source.display = product.display;
        source.source_column = crop.window.col; source.source_columns = shape.cols;
        source.horizontal_period_pixels = 360 * crop.map.view.pixels.cols / (crop.map.view.bounds.right - crop.map.view.bounds.left);
        for (const auto& plane : product.frame.planes) {
            source.planes.push_back(plane.values.view().window(crop.window)); source.valid.push_back(plane.valid.view().window(crop.window));
        }
        RasterOptions options;
        options.layout = crop.map.layout; options.bit_depth = bits; options.exact_coverage = false;
        auto rendered = rasterize_swaths(std::span(&source,1),options,context);
        save_frame(rendered,crop.map,product,request,path,context);
    } else {
        if (request.grid && !disk) throw std::invalid_argument("Geographic grid requires disk navigation");
        const auto width = request.width ? std::size_t(request.width) : shape.cols;
        const auto raw_height = double(shape.rows) * width / shape.cols;
        if (width > 16000 || raw_height > 16000) throw std::invalid_argument("Disk image dimensions exceed supported limits");
        const Shape pixels{std::max<std::size_t>(1,std::size_t(raw_height)),width};
        const auto layout = LayoutTransform::make(pixels,{.baseline_map_width = double(shape.cols),.right = bits == 16 ? 100.0 : 300.0});
        MapPlan map{{{}, {0,double(shape.rows),0,double(shape.cols)},pixels},layout,disk ? std::optional(*disk) : std::nullopt,true};
        ImageRasterSource source;
        source.cells = shape; source.display = product.display;
        for (const auto& plane : product.frame.planes) {
            source.planes.push_back(plane.values.view()); source.valid.push_back(plane.valid.view());
        }
        RasterOptions options;
        options.layout = layout; options.bit_depth = bits;
        auto rendered = rasterize_image(source,options,context);
        save_frame(rendered,map,product,request,path,context);
    }
}

}
