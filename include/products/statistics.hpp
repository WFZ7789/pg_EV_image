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
    std::vector<std::uint64_t> histogram_;
    std::uint64_t samples_ = 0;
    double low_ = std::numeric_limits<double>::infinity(),high_ = -std::numeric_limits<double>::infinity();
};

}
