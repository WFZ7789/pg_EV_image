/// SPDX-License-Identifier: MIT

#include "core/calibration.hpp"

#include <cmath>

namespace {

void require(bool condition) {
    if (!condition) throw std::runtime_error("Calibration check failed");
}

}

int main() {
    using namespace fy::core;
    ExecutionContext context({.threads = 4});
    SampleBlock linear{0, {0,0,{1,4}}, Raster<float>({1,4},{0,1,2,-9999}),
                       Raster<std::uint8_t>({1,4},{1,1,1,0})};
    CalibrationDescriptor calibration;
    calibration.kind = CalibrationKind::linear;
    calibration.coefficients = {2,3};
    calibrate(linear, calibration, context);
    require(linear.values.values()[0] == 3 && linear.values.values()[2] == 7);
    require(std::isnan(linear.values.values()[3]) && !linear.valid.values()[3]);
    SampleBlock masked{0,{0,0,{1,2}},Raster<float>({1,2},{100,200}),Raster<std::uint8_t>({1,2},{1,0})};
    calibrate(masked,calibration,context,InvalidSamplePolicy::retain_finite);
    require(masked.values.values()[0] == 203 && masked.values.values()[1] == 403);
    require(masked.valid.values()[0] && !masked.valid.values()[1]);
    SampleBlock polynomial{0,{0,0,{1,2}},Raster<float>({1,2},{2,3}),Raster<std::uint8_t>({1,2},{1,1})};
    calibration.kind = CalibrationKind::polynomial;
    calibration.coefficients = {2,1,3,4,5};
    calibrate(polynomial, calibration, context);
    require(polynomial.values.values()[0] == 148 && polynomial.values.values()[1] == 276);
    SampleBlock physical{0,{0,0,{1,2}},Raster<float>({1,2},{50,-50}),Raster<std::uint8_t>({1,2},{1,1})};
    calibration.kind = CalibrationKind::physical;
    calibration.coefficients = {1,0,931.601394,1,0};
    calibrate(physical, calibration, context);
    require(physical.valid.values()[0] && physical.values.values()[0] > 200 && physical.values.values()[0] < 350);
    require(!physical.valid.values()[1] && std::isnan(physical.values.values()[1]));
    SampleBlock lookup{0,{0,0,{1,4}},Raster<float>({1,4},{0,1,2,0.5}),Raster<std::uint8_t>({1,4},{1,1,1,1})};
    calibration.kind = CalibrationKind::lookup;
    calibration.coefficients = {2,4};
    calibrate(lookup, calibration, context);
    require(lookup.values.values()[0] == 2 && lookup.values.values()[1] == 4);
    require(!lookup.valid.values()[2] && !lookup.valid.values()[3]);
    calibration.kind = CalibrationKind::linear;
    calibration.coefficients = {1};
    bool rejected = false;
    try { calibrate(linear, calibration, context); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
}
