/// SPDX-License-Identifier: MIT

#pragma once

#include "pipeline.hpp"
#include "products/execute.hpp"

namespace fy {

struct ProductRenderOptions {
    int bit_depth = 8;
    bool region_already_selected = false;
};

/// @brief 按已有几何能力成图，不读取载荷名称或执行产品公式。
void render_product(const Request& request,const products::LoadedProduct& product,core::ExecutionContext& context,
                    const ProductRenderOptions& options = {});

}
