/// SPDX-License-Identifier: MIT

#pragma once

#include "products/swath.hpp"

namespace fy::products {

ProductDefinition pmr_product(const core::SensorMetadata& metadata, std::span<const core::BandDescriptor> bands,
                             const ProductRequest& request);

}
