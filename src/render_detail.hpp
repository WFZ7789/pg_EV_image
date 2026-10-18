/// SPDX-License-Identifier: MIT

#pragma once

#include "core/render.hpp"
#include "pipeline.hpp"
#include "products/swath.hpp"

namespace fy::render_detail {

struct MapPlan {
    core::MapView view;
    core::LayoutTransform layout;
    std::optional<core::NativeDiskGeometry> disk = {};
    bool image_axes = false;
};

struct DiskCrop {
    MapPlan map;
    core::Window window;
};

void validate_request(const Request& request,bool require_input = true);
std::filesystem::path output_path(const Request& request);
MapPlan plan_map(const core::SwathGeometry& geometry,const Request& request,bool cropped);
DiskCrop plan_disk_crop(const core::NativeDiskGeometry& geometry,const Request& request,core::ExecutionContext& context);
void save_frame(core::RasterizedFrame& rendered,const MapPlan& map,const products::PreparedProduct& product,
                const Request& request,const std::filesystem::path& path,core::ExecutionContext& context);

}
