/// SPDX-License-Identifier: MIT

#pragma once

#include "products/execute.hpp"

namespace fy::products {

ProductDefinition giirs_product(const core::SensorMetadata& metadata,std::span<const core::BandDescriptor> bands,
                               const ProductRequest& request,bool sequence_calibration = false);
core::ProductPlan giirs_sequence_plan(const core::SensorMetadata& metadata,std::span<const core::BandDescriptor> bands);

/// @brief 每族仅保留一帧内存；数值缓存确定共同显示范围后按 MW、LW 顺序消费。
void stream_giirs_sequence(core::SensorSession& session,core::ExecutionContext& context,
                          const std::function<void(const PreparedProduct&,const core::FramePlan&)>& consume);

}
