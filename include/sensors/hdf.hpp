/// SPDX-License-Identifier: MIT

#pragma once

#include "core/adapter.hpp"
#include "core/profile.hpp"

namespace fy::sensors {

inline constexpr int solar_zenith = -1001;
inline constexpr int solar_azimuth = -1002;
inline constexpr int low_light = -1003;
inline constexpr int satellite_zenith = -1004;
inline constexpr int auxiliary_band_offset = 1000;
inline constexpr int sounding_band_offset = 100;
inline constexpr int giirs_lw_offset = 10000;
inline constexpr int giirs_mw_offset = 20000;

struct HdfSummary {
    std::string satellite;
    std::string sensor;
    std::string alias;
};

/// @brief 在串行 I/O 通道内读取检查命令所需的元数据，不要求数据与 GEO 同文件。
HdfSummary read_hdf_summary(const std::filesystem::path& path);

/// @brief 在串行 I/O 通道内按元数据及实际数据集识别载荷。
std::string identify_hdf_adapter(const core::SourceFiles& sources);

/// @brief 注册读取原始窗口的适配器；定标描述供 CPU 任务执行。
void register_hdf_adapters(core::AdapterRegistry& registry,
                           std::optional<core::ProfileReport> report = std::nullopt);

}
