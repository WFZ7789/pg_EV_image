/// SPDX-License-Identifier: MIT

#pragma once

#include "products/swath.hpp"

namespace fy::products {

/// @brief 原地将红、近红外、蓝反射率转换为既有 AGRI 日间显示 RGB，保留有效性。
void correct_agri_day(core::ScalarPlane& red,core::ScalarPlane& near_infrared,core::ScalarPlane& blue,
                      const core::ScalarPlane& solar_zenith,const core::ScalarPlane& satellite_zenith,
                      const std::string& satellite,double distance,core::ExecutionContext& context);

ProductDefinition fy4_product(const core::SensorMetadata& metadata, std::span<const core::BandDescriptor> bands,
                               const ProductRequest& request);

}
