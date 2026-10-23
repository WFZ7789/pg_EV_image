/// SPDX-License-Identifier: MIT

#pragma once

#include "pipeline.hpp"
#include "products/execute.hpp"

namespace fy {

struct ProductRenderOptions {
    int bit_depth = 8;
    bool region_already_selected = false;
    bool cache_swaths = false;
};

/// @brief 按已有几何能力成图，不读取载荷名称或执行产品公式。
void render_product(const Request& request,const products::LoadedProduct& product,core::ExecutionContext& context,
                    const ProductRenderOptions& options = {});

/// @brief 将相同产品的独立扫描带按原生定位合到同一画布。
void render_swath_products(const Request& request,std::span<const products::LoadedProduct> products,
                           core::ExecutionContext& context,const ProductRenderOptions& options = {});

}
