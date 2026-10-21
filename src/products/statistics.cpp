/// SPDX-License-Identifier: MIT

#include "products/statistics.hpp"

#include <algorithm>
#include <cmath>

namespace fy::products {

SharedDisplayStatistics::SharedDisplayStatistics(std::string mode,core::ProfileReport& profile) : mode_(std::move(mode)),profile_(profile) {
    if (mode_ == "RT") {
        memory_.emplace(profile.reserve(core::BufferKind::statistics,4096 * sizeof(NeutralBin)));
        neutral_histogram_.resize(4096);
    }
    if (mode_ == "RY" || mode_ == "RMUL") {
        memory_.emplace(profile.reserve(core::BufferKind::statistics,65536 * sizeof(std::uint64_t)));
        histogram_.resize(65536);
    }
}

void SharedDisplayStatistics::add(const PreparedProduct& product) {
    if (mode_ == "RT") {
        if (product.frame.planes.size() != 3) throw std::invalid_argument("RT shared statistics require three reflection planes");
        for (std::size_t i = 0; i < product.frame.planes.front().values.values().size(); ++i) {
            std::array<double,3> rgb{};
            bool complete = true;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto& plane = product.frame.planes[2 - channel];
                const auto value = plane.values.values()[i];
                if (!plane.valid.values()[i] || !std::isfinite(value) || value > 2.5f) { complete = false; continue; }
                if (samples_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Shared statistics sample overflow");
                ++samples_; rgb[channel] = value;
                rgb_low_[channel] = std::min(rgb_low_[channel],double(value));
                rgb_high_[channel] = std::max(rgb_high_[channel],double(value));
            }
            if (!complete) continue;
            const auto [low,high] = std::minmax_element(rgb.begin(),rgb.end());
            if (*high < 0.2 || *low < *high * 0.8) continue;
            const double brightness = (rgb[0] + rgb[1] + rgb[2]) / 3;
            const auto bin = std::min(neutral_histogram_.size() - 1,std::size_t(brightness / 2.5 * (neutral_histogram_.size() - 1)));
            auto& neutral = neutral_histogram_[bin];
            ++neutral.count; ++neutral_samples_;
            for (std::size_t channel = 0; channel < 3; ++channel) neutral.sum[channel] += rgb[channel];
        }
        return;
    }
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
    if (mode_ != "RT" && mode_ != "E" && histogram_.empty()) return {};
    if (!samples_) throw std::invalid_argument("Product has no finite shared-statistics samples");
    if (mode_ == "RT") {
        auto phase = profile_.phase("white_balance");
        core::DisplayScale result{INFINITY,-INFINITY};
        std::array<double,3> sum{};
        std::uint64_t selected = 0;
        const auto needed = neutral_samples_ - neutral_samples_ * 9 / 10;
        for (std::size_t i = neutral_histogram_.size(); i > 0 && selected < needed; --i) {
            const auto& bin = neutral_histogram_[i - 1];
            selected += bin.count;
            for (std::size_t channel = 0; channel < 3; ++channel) sum[channel] += bin.sum[channel];
        }
        const double target = *std::min_element(sum.begin(),sum.end());
        for (std::size_t channel = 0; channel < 3; ++channel) {
            if (target > 0) result.rgb_gain[channel] = target / sum[channel];
            if (!std::isfinite(rgb_low_[channel])) continue;
            result.low = std::min(result.low,rgb_low_[channel] * result.rgb_gain[channel]);
            result.high = std::max(result.high,rgb_high_[channel] * result.rgb_gain[channel]);
        }
        if (result.high - result.low <= 8 * std::numeric_limits<double>::epsilon() * std::max(1.0,std::abs(result.high)))
            result.high = result.low + 1;
        return result;
    }
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
