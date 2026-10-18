/// SPDX-License-Identifier: MIT

#pragma once

#include "pipeline.hpp"
#include "products/registry.hpp"

namespace fy {

void render_native_scene(const Request& request,const std::string& adapter,core::ExecutionContext& context);
void render_registered_scene(const Request& request,const std::string& adapter,core::AdapterRegistry& sensors,
                             products::ProductRegistry& products,core::ExecutionContext& context);
void render_native_mosaic(const Request& request, core::ExecutionContext& context);
void render_native_fy4(const Request& request, core::ExecutionContext& context);
void render_native_giirs(const Request& request, core::ExecutionContext& context);

}
