/// SPDX-License-Identifier: MIT

#pragma once

#include "core/render.hpp"

namespace fy::products {

struct ProductRequest {
    std::string mode;
    int band = 0;
    std::string color;
    core::MapProjection projection;
    bool mosaic = false;
    std::string scan = "A";
};

struct PreparedProduct {
    core::ProductFrame frame;
    core::DisplayDescriptor display;
    std::vector<std::string> labels;
    std::string extra_label;
    std::optional<std::size_t> shared_percentile_plane;
};

struct ProductDefinition {
    core::ProductPlan plan;
    std::function<PreparedProduct(std::vector<core::ScalarPlane>&&,
                                  std::shared_ptr<const core::NativeGeometry>, core::ExecutionContext&)> prepare;
};

ProductDefinition mersi_product(const core::SensorMetadata& metadata, std::span<const core::BandDescriptor> bands,
                           const ProductRequest& request);
ProductDefinition mwri_product(const core::SensorMetadata& metadata, std::span<const core::BandDescriptor> bands,
                          const ProductRequest& request);
ProductDefinition amsr3_product(const core::SensorMetadata& metadata, std::span<const core::BandDescriptor> bands,
                               const ProductRequest& request);

float percentile(std::span<const float> values, float percent, core::ExecutionContext& context);

}
