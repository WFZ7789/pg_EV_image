/// SPDX-License-Identifier: MIT

#include "products/statistics.hpp"

#include <algorithm>
#include <cmath>

namespace fy::products {

SharedDisplayStatistics::SharedDisplayStatistics(std::string mode,core::ProfileReport& profile) : mode_(std::move(mode)) {
    if (mode_ == "RT" || mode_ == "RY" || mode_ == "RMUL") {
        memory_.emplace(profile.reserve(core::BufferKind::statistics,65536 * sizeof(std::uint64_t)));
        histogram_.resize(65536);
    }
}

void SharedDisplayStatistics::add(const PreparedProduct& product) {
    if (mode_ != "E" && histogram_.empty()) return;
    const auto index = mode_ == "E" ? 0 : product.shared_percentile_plane.value_or(std::numeric_limits<std::size_t>::max());
    if (index >= product.frame.planes.size()) throw std::invalid_argument("Missing shared-statistics product plane");
    for (const auto value : product.frame.planes[index].values.values()) {
        if (!std::isfinite(value)) continue;
        if (samples_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Shared statistics sample overflow");
        ++samples_;
        if (histogram_.empty()) {
            low_ = std::min(low_,double(value)); high_ = std::max(high_,double(value));
        } else {
            const auto bin = static_cast<std::size_t>(std::clamp(double(value),0.0,8.0) * 65535.0 / 8.0);
            ++histogram_[bin];
        }
    }
}

std::optional<core::DisplayScale> SharedDisplayStatistics::scale() const {
    if (mode_ != "E" && histogram_.empty()) return {};
    if (!samples_) throw std::invalid_argument("Product has no finite shared-statistics samples");
    if (histogram_.empty()) return core::DisplayScale{std::max(low_,-105.0),std::min(high_,40.0)};
    const auto rank = samples_ - samples_ / 100;
    std::uint64_t total = 0;
    for (std::size_t bin = 0; bin < histogram_.size(); ++bin) {
        total += histogram_[bin];
        if (total < rank) continue;
        if (bin + 1 == histogram_.size()) throw std::invalid_argument("Shared reflection stretch exceeds 8");
        return core::DisplayScale{0,std::max(0.001f,float(bin * 8.0 / 65535.0))};
    }
    throw std::logic_error("Shared statistics histogram is incomplete");
}

}
