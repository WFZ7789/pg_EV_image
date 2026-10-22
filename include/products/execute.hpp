/// SPDX-License-Identifier: MIT

#pragma once

#include "products/swath.hpp"

namespace fy::products {

struct LoadedProduct {
    core::ProfileReport::BufferReservation memory;
    PreparedProduct product;
    core::Window source_window;
    core::Shape source_shape;
};

struct GeographicRegion {
    double latitude = 0;
    double longitude = 0;
    double half_range = 7.5;
};

struct RegionOutsideCoverage : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

/// @note 仅在同一 SensorSession、同一任务内复用。
struct ProductLoadCache {
    std::map<int,std::shared_ptr<const core::ScalarPlane>> planes;
    std::map<std::string,std::shared_ptr<const core::NativeGeometry>> geometry;
    std::vector<core::ProfileReport::BufferReservation> memory;
};

LoadedProduct load_product(core::SensorSession& session, const ProductDefinition& definition,
                                 core::ExecutionContext& context,
                                 std::optional<GeographicRegion> region = {}, ProductLoadCache* cache = nullptr);

}
