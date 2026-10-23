/// SPDX-License-Identifier: MIT

#include "../src/products/common.hpp"

#include <source_location>

namespace {

using namespace fy::core;
using namespace fy::products::detail;

void require(bool condition,std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("Scan shadow failed at line "+std::to_string(location.line()));
}

void check(float origin,std::size_t first_row) {
    const Shape shape{192-first_row,64};
    SwathGeometry geometry{Raster<float>(shape),Raster<float>(shape),Raster<std::uint8_t>(shape),24,first_row};
    ScalarPlane temperature{"degC",Raster<float>(shape),Raster<std::uint8_t>(shape)};
    ScalarPlane zenith{"degree",Raster<float>(shape),Raster<std::uint8_t>(shape)};
    ScalarPlane azimuth{"degree",Raster<float>(shape),Raster<std::uint8_t>(shape)};
    for (std::size_t row = 0; row < shape.rows; ++row)
        for (std::size_t col = 0; col < shape.cols; ++col) {
            const auto scan = (first_row+row)/24,detector = (first_row+row)%24,i = row*shape.cols+col;
            const double lat = 10+(scan*14+detector)*.01+col*.0005;
            const double lon = origin+col*.02+detector*.003+scan*.002;
            geometry.latitude.values()[i] = float(lat);
            geometry.longitude.values()[i] = float(std::remainder(lon,360.));
            geometry.valid.values()[i] = 1;
            temperature.values.values()[i] = float(250+100*(lat-10)+50*(lon-origin));
            temperature.valid.values()[i] = detector >= 4 && detector < 20;
            zenith.values.values()[i] = 60; azimuth.values.values()[i] = 90;
            zenith.valid.values()[i] = azimuth.valid.values()[i] = 1;
        }
    ScanTemperatureSampler sampler(temperature,geometry);
    const auto row = 24*3+15-first_row;
    for (int dy : {-30,-8,0,8,20,30}) {
        const auto result = sampler.offset(row,30,dy,3);
        require(bool(result));
        const double lat = 10+(3*14+15+dy)*.01+33*.0005;
        const double lon = origin+33*.02+(15+dy)*.003+3*.002;
        require(std::abs(*result-(250+100*(lat-10)+50*(lon-origin))) < .03);
    }
    require(!sampler.offset(row,0,1,-1));
    require(!sampler.offset(4-first_row%4,30,-100,0));
    std::vector<float> reference;
    for (int threads : {1,4}) {
        ExecutionContext context({.threads=threads});
        auto result = shadow(temperature,zenith,azimuth,context,2,24,first_row,&geometry);
        const auto truncated = shadow(temperature,zenith,azimuth,context,2,24,first_row);
        const auto seam = (3*24+19-first_row)*shape.cols+30;
        require(std::abs(truncated.values.values()[seam]) < 1e-6);
        require(std::abs(result.values.values()[seam]+2.3f) < .03);
        for (std::size_t scan = 1; scan < 6; ++scan)
            for (std::size_t detector = 4; detector < 20; ++detector) {
                const auto i = (scan*24+detector-first_row)*shape.cols+30;
                require(result.valid.values()[i]);
                require(std::abs(result.values.values()[i]+2.3f) < .03);
            }
        for (std::size_t i = 0; i < shape.elements(); ++i) {
            require(bool(result.valid.values()[i]) == bool(temperature.valid.values()[i]));
            if (!reference.empty()) require(result.values.values()[i] == reference[i]);
        }
        reference.assign(result.values.values().begin(),result.values.values().end());
        std::fill(azimuth.values.values().begin(),azimuth.values.values().end(),0);
        auto old = shadow(temperature,zenith,azimuth,context,2,24,first_row);
        auto same = shadow(temperature,zenith,azimuth,context,2,24,first_row,&geometry);
        for (std::size_t i = 0; i < shape.elements(); ++i)
            if (temperature.valid.values()[i]) require(old.values.values()[i] == same.values.values()[i]);
        std::fill(azimuth.values.values().begin(),azimuth.values.values().end(),90);
    }
    std::fill(temperature.values.values().begin(),temperature.values.values().end(),250);
    ExecutionContext context({.threads=1});
    auto flat = shadow(temperature,zenith,azimuth,context,2,24,first_row,&geometry);
    for (const auto v : flat.values.values()) require(v == 0);
}

}

int main() {
    for (float origin : {120.f,179.5f})
        for (std::size_t first_row : {0,5}) check(origin,first_row);
}
