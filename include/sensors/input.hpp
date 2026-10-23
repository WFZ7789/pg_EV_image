/// SPDX-License-Identifier: MIT

#pragma once

#include "core/execution.hpp"

namespace fy::sensors {

/// @brief 将官方 ZIP 中唯一的 NetCDF 解码到任务私有目录，普通文件直接返回。
std::filesystem::path resolve_data_input(const std::filesystem::path& path,const core::ScratchDirectory& scratch);

}
