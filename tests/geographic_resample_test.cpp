/// SPDX-License-Identifier: MIT

#include "core/resample.hpp"

#include <cmath>
#include <source_location>

namespace {
void require(bool condition,std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("Geographic resampling failed at "+std::to_string(location.line()));
}
}

int main() {
    using namespace fy::core;
    for (const auto center : {0.f,179.9f}) {
        SwathGeometry source{Raster<float>({3,3}),Raster<float>({3,3}),Raster<std::uint8_t>({3,3})};
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t col = 0; col < 3; ++col) {
                source.latitude.view().at(row,col) = 10+float(row)*.1f;
                source.longitude.view().at(row,col) = std::remainder(center+float(col)*.1f,360.f);
                source.valid.view().at(row,col) = 1;
            }
        ScalarPlane plane{"K",Raster<float>({3,3},{200,210,220,220,230,240,240,250,260}),Raster<std::uint8_t>({3,3},{1,1,1,1,1,1,1,1,1})};
        SwathGeometry target{Raster<float>({1,3},{10.05f,10.15f,12}),
            Raster<float>({1,3},{std::remainder(center+.05f,360.f),std::remainder(center+.15f,360.f),center}),
            Raster<std::uint8_t>({1,3},{1,1,1})};
        std::vector<float> reference;
        for (int threads : {1,4,0}) {
            ExecutionContext context({.threads = threads});
            auto weights = geographic_weights(source,target,context);
            auto result = resample_geographic(plane,weights,context);
            require(result.valid.values()[0] && result.valid.values()[1] && !result.valid.values()[2]);
            require(std::abs(result.values.values()[0]-215) < .02);
            require(std::abs(result.values.values()[1]-245) < .02);
            require(std::isnan(result.values.values()[2]));
            if (reference.empty()) reference.assign(result.values.values().begin(),result.values.values().end());
            else for (int i = 0; i < 2; ++i) require(reference[i] == result.values.values()[i]);
            auto identity = resample_geographic(plane,geographic_weights(source,source,context),context);
            for (std::size_t i = 0; i < 9; ++i) require(std::abs(identity.values.values()[i]-plane.values.values()[i]) < 1e-4);
            plane.valid.values()[4] = 0;
            auto masked = resample_geographic(plane,weights,context);
            require(!masked.valid.values()[0] && !masked.valid.values()[1]);
            plane.valid.values()[4] = 1;
            source.valid.values()[4] = 0;
            auto hole = resample_geographic(plane,geographic_weights(source,target,context),context);
            require(!hole.valid.values()[0] && !hole.valid.values()[1]);
            source.valid.values()[4] = 1;
        }
    }
    ExecutionContext context({.threads = 1});
    SwathGeometry polar{Raster<float>({2,2},{89.8f,89.8f,89.9f,89.9f}),
        Raster<float>({2,2},{-20,20,-20,20}),Raster<std::uint8_t>({2,2},{1,1,1,1})};
    SwathGeometry target{Raster<float>({1,1},{89.85f}),Raster<float>({1,1},{0}),Raster<std::uint8_t>({1,1},{1})};
    ScalarPlane constant{"K",Raster<float>({2,2},{250,250,250,250}),Raster<std::uint8_t>({2,2},{1,1,1,1})};
    auto result = resample_geographic(constant,geographic_weights(polar,target,context),context);
    require(result.valid.values()[0] && std::abs(result.values.values()[0]-250) < 1e-4);
}
