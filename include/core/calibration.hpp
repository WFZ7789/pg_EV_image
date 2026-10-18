/// SPDX-License-Identifier: MIT

#pragma once

#include "core/execution.hpp"

namespace fy::core {

/// @brief 原地定标，保留旧公式的 float 中间舍入，独立于 HDF5 I/O。
void calibrate(SampleBlock& block, const CalibrationDescriptor& calibration, ExecutionContext& context,
               InvalidSamplePolicy invalid = InvalidSamplePolicy::nan);

}
