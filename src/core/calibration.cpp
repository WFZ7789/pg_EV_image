/// SPDX-License-Identifier: MIT

#include "core/calibration.hpp"
#include "products/phys.hpp"

#include <cmath>

namespace fy::core {

void calibrate(SampleBlock& block, const CalibrationDescriptor& calibration, ExecutionContext& context,
               InvalidSamplePolicy invalid) {
    block.validate();
    const auto& coefficients = calibration.coefficients;
    std::size_t required = 0;
    switch (calibration.kind) {
        case CalibrationKind::identity: break;
        case CalibrationKind::linear: required = 2; break;
        case CalibrationKind::polynomial: required = 5; break;
        case CalibrationKind::physical: required = 5; break;
        case CalibrationKind::lookup: required = 1; break;
    }
    if ((calibration.kind != CalibrationKind::lookup && coefficients.size() != required) ||
        (calibration.kind == CalibrationKind::lookup && coefficients.empty()))
        throw std::invalid_argument("Incorrect calibration coefficient count");
    if (calibration.kind != CalibrationKind::lookup)
        for (double coefficient : coefficients)
            if (!std::isfinite(coefficient)) throw std::invalid_argument("Nonfinite calibration coefficient");
    if (calibration.kind == CalibrationKind::physical && coefficients[2] <= 0)
        throw std::invalid_argument("Invalid Planck wavenumber");
    auto phase = context.profile().phase("calibration");
    auto values = block.values.values();
    auto valid = block.valid.values();
    const auto shape = block.values.shape();
    context.parallel_rows(shape.rows, [&](std::size_t begin, std::size_t end) {
        for (auto i = begin * shape.cols; i < end * shape.cols; ++i) {
            if (!valid[i] && invalid == InvalidSamplePolicy::nan) { values[i] = NAN; continue; }
            float value = values[i];
            if (calibration.kind == CalibrationKind::lookup) {
                if (value < 0 || static_cast<double>(value) >= coefficients.size() || std::floor(value) != value)
                    value = NAN;
                else value = static_cast<float>(coefficients[static_cast<std::size_t>(value)]);
            } else if (calibration.kind != CalibrationKind::identity) {
                value = value * static_cast<float>(coefficients[0]) + static_cast<float>(coefficients[1]);
                if (calibration.kind == CalibrationKind::polynomial)
                    value = static_cast<float>(coefficients[4]) * value * value +
                            static_cast<float>(coefficients[3]) * value + static_cast<float>(coefficients[2]);
                else if (calibration.kind == CalibrationKind::physical)
                    value = static_cast<float>(brtemp(value, coefficients[2]) *
                                               static_cast<float>(coefficients[3]) + static_cast<float>(coefficients[4]));
            }
            bool calibrated_valid = std::isfinite(value);
            for (const auto fill : calibration.fill_values)
                if (value == static_cast<float>(fill)) calibrated_valid = false;
            valid[i] = valid[i] && calibrated_valid;
            values[i] = std::isfinite(value) && (valid[i] || invalid == InvalidSamplePolicy::retain_finite) ? value : NAN;
        }
    });
    context.profile().record_computed(shape.elements());
}

}
