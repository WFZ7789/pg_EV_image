/// SPDX-License-Identifier: MIT

#pragma once

#include "core/calibration.hpp"

namespace fy::core {

struct SampleTile {
    ProfileReport::BufferReservation memory;
    SampleBlock samples;

    SampleTile(SampleBlock block, ProfileReport::BufferReservation allocation)
        : memory(std::move(allocation)), samples(std::move(block)) {}
};

/// @brief 两个拥有数据的缓冲衔接串行读取与并行定标，异常退出前排空读取。
/// @param calibration_override 显式覆盖波段定标，单位与统计口径由产品依赖声明。
void stream_calibrated(SensorSession& session, int band, Window region, ExecutionContext& context,
                        const std::function<void(SampleTile&&)>& consume, std::size_t block_rows = 0,
                        InvalidSamplePolicy invalid = InvalidSamplePolicy::nan,
                        std::optional<CalibrationDescriptor> calibration_override = {});

}
