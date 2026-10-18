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

LoadedProduct load_product(core::SensorSession& session, const ProductDefinition& definition,
                                 core::ExecutionContext& context,
                                 std::optional<GeographicRegion> region = {});

}
