/// SPDX-License-Identifier: MIT

#pragma once

#include "products/swath.hpp"

namespace fy::products {

class SharedDisplayStatistics {
public:
    SharedDisplayStatistics(std::string mode,core::ProfileReport& profile);
    void add(const PreparedProduct& product);
    std::optional<core::DisplayScale> scale() const;
private:
    std::optional<core::ProfileReport::BufferReservation> memory_;
    std::string mode_;
    core::ProfileReport& profile_;
    std::vector<std::uint64_t> histogram_;
    struct NeutralBin {
        std::uint64_t count = 0;
        std::array<double,3> sum{};
    };
    std::vector<NeutralBin> neutral_histogram_;
    std::uint64_t neutral_samples_ = 0;
    static constexpr double infinity = std::numeric_limits<double>::infinity();
    std::array<double,3> rgb_low_{infinity,infinity,infinity},rgb_high_{-infinity,-infinity,-infinity};
    std::uint64_t samples_ = 0;
    double low_ = std::numeric_limits<double>::infinity(),high_ = -std::numeric_limits<double>::infinity();
};

}
