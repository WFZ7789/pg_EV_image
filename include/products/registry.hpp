/// SPDX-License-Identifier: MIT

#pragma once

#include "products/swath.hpp"

#include <map>

namespace fy::products {

class ProductRegistry {
public:
    using Factory = std::function<ProductDefinition(const core::SensorMetadata&,std::span<const core::BandDescriptor>,
                                                     const ProductRequest&)>;
    void add(std::string key,Factory factory);
    ProductDefinition plan(const std::string& key,const core::SensorMetadata& metadata,
                           std::span<const core::BandDescriptor> bands,const ProductRequest& request) const;
private:
    std::map<std::string,Factory> factories_;
};

void register_products(ProductRegistry& registry);

}
