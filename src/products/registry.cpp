/// SPDX-License-Identifier: MIT

#include "products/registry.hpp"
#include "products/fy4.hpp"
#include "products/giirs.hpp"
#include "products/pmr.hpp"

namespace fy::products {

void ProductRegistry::add(std::string key,Factory factory) {
    if (key.empty() || !factory) throw std::invalid_argument("Invalid product registration");
    if (!factories_.emplace(std::move(key),std::move(factory)).second)
        throw std::invalid_argument("Duplicate product registration");
}

ProductDefinition ProductRegistry::plan(const std::string& key,const core::SensorMetadata& metadata,
                                       std::span<const core::BandDescriptor> bands,const ProductRequest& request) const {
    const auto found = factories_.find(key);
    if (found == factories_.end()) throw std::invalid_argument("Unknown product registration: " + key);
    return found->second(metadata,bands,request);
}

void register_products(ProductRegistry& registry) {
    registry.add("mersi",mersi_product);
    registry.add("mwri",mwri_product);
    registry.add("pmr",pmr_product);
    registry.add("fy4",fy4_product);
    registry.add("giirs",[](const auto& metadata,auto bands,const auto& request) {
        return giirs_product(metadata,bands,request);
    });
}

}
