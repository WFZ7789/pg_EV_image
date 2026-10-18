/// SPDX-License-Identifier: MIT

#pragma once

#include "products/execute.hpp"

namespace fy::products {

/// @brief 任务私有的数值与顶点缓存；借用的 source 必须先于缓存析构。
class CachedSwath {
public:
    CachedSwath(const std::filesystem::path& prefix,const LoadedProduct& product,
                 core::ExecutionContext& context,std::size_t block_size = 128);
    ~CachedSwath();
    CachedSwath(const CachedSwath&) = delete;
    CachedSwath& operator=(const CachedSwath&) = delete;

    core::DisplayDescriptor& display();
    const std::vector<std::string>& labels() const;
    const std::string& extra_label() const;
    void project(const core::MapView& map);
    core::SwathRasterSource source(std::uint64_t order) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
