/// SPDX-License-Identifier: MIT

#include "products/pmr.hpp"
#include "common.hpp"

namespace fy::products {

using namespace core;
using namespace detail;

ProductDefinition pmr_product(const SensorMetadata& metadata, std::span<const BandDescriptor> bands,
                             const ProductRequest& request) {
    if (request.mode != "PMR" || metadata.sensor.find("PMR") == std::string::npos)
        throw std::invalid_argument("Unknown PMR product: " + request.mode);
    const auto& band = band_at(bands,1);
    if (band.calibrated_unit != "dBZ") throw std::invalid_argument("PMR reflectivity must use dBZ");
    ProductDefinition definition;
    definition.plan.regional_default_half_range = 7.5;
    definition.plan.mode = "PMR";
    definition.plan.calculation_grid = band.grid;
    definition.plan.inputs.push_back({1,band.grid});
    definition.plan.frames.push_back({.name = "PMR",.units = {"dBZ"}});
    StatisticsSpec maximum;
    maximum.operation = StatisticsOperation::minmax;
    maximum.field = "surface_reflectivity";
    maximum.before_crop = false;
    maximum.include_masked_finite = true;
    definition.plan.statistics.push_back(maximum);
    definition.plan.validate(metadata,bands);
    definition.prepare = [metadata,description = band.name](std::vector<ScalarPlane>&& inputs,
                            std::shared_ptr<const NativeGeometry> geometry,ExecutionContext& context) {
        auto phase = context.profile().phase("product");
        if (!geometry || !std::holds_alternative<SwathGeometry>(*geometry))
            throw std::invalid_argument("PMR requires swath geolocation");
        validate_planes(inputs,1,geometry_shape(*geometry));
        float high = -INFINITY;
        for (const float value : inputs.front().values.values())
            if (std::isfinite(value)) high = std::max(high,value);
        if (!std::isfinite(high)) throw std::invalid_argument("PMR region has no finite samples");
        context.profile().record_computed(inputs.front().values.shape().elements());
        PreparedProduct product;
        product.frame.name = "PMR";
        product.frame.geometry = std::move(geometry);
        product.frame.planes = std::move(inputs);
        product.labels = labels(metadata,description,false);
        product.display = scalar_display("",palette::color_ir_ca,{0,high});
        product.frame.validate();
        return product;
    };
    return definition;
}

}
