/// SPDX-License-Identifier: MIT

#include "core/resample.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace fy::core {
namespace {

using Vector = std::array<double,3>;
Vector cross(const Vector& a,const Vector& b) {
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
}
double dot(const Vector& a,const Vector& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
double distance(const Vector& a,const Vector& b) {
    double sum = 0;
    for (int i = 0; i < 3; ++i) sum += (a[i]-b[i])*(a[i]-b[i]);
    return std::sqrt(sum);
}
Vector position(float latitude,float longitude) {
    const auto lat = latitude * std::numbers::pi / 180,lon = longitude * std::numbers::pi / 180;
    return {std::cos(lat)*std::cos(lon),std::cos(lat)*std::sin(lon),std::sin(lat)};
}
std::uint64_t key(int x,int y,int z) {
    return (std::uint64_t(x) << 40) | (std::uint64_t(y) << 20) | std::uint64_t(z);
}

}

Raster<GeographicWeights> geographic_weights(const SwathGeometry& source,const SwathGeometry& target,
                                            ExecutionContext& context) {
    source.validate(); target.validate();
    const auto shape = source.latitude.shape(),output = target.latitude.shape();
    if (shape.elements() > std::numeric_limits<std::uint32_t>::max())
        throw std::overflow_error("Geographic source exceeds compact index capacity");
    auto phase = context.profile().phase("geographic_weights");
    auto workspace = context.profile().reserve(BufferKind::statistics,shape.elements()*sizeof(Vector));
    std::vector<Vector> points(shape.elements());
    for (std::size_t i = 0; i < points.size(); ++i)
        if (source.valid.values()[i]) points[i] = position(source.latitude.values()[i],source.longitude.values()[i]);
    std::vector<double> spacing;
    const auto stride = std::max<std::size_t>(1,shape.elements()/4096);
    for (std::size_t i = 0; i < points.size(); i += stride) {
        if (!source.valid.values()[i]) continue;
        for (auto j : {i+1,i+shape.cols})
            if (j < points.size() && (j != i+1 || j/shape.cols == i/shape.cols) && source.valid.values()[j]) {
                auto d = distance(points[i],points[j]);
                if (d > 1e-10) spacing.push_back(d);
            }
    }
    Raster<GeographicWeights> result(output);
    if (spacing.empty() || shape.rows < 2 || shape.cols < 2) return result;
    std::ranges::sort(spacing);
    const auto typical = spacing[spacing.size()/2];
    if ((shape.rows-1)*(shape.cols-1) > std::numeric_limits<std::uint32_t>::max()/2)
        throw std::overflow_error("Geographic triangle count exceeds compact index capacity");
    const auto step = std::clamp(typical*2,.002,.25);
    const auto bin = [step](double v) { return int(std::floor((v+1.5)/step)); };
    using Triangle = std::array<std::uint32_t,3>;
    std::vector<Triangle> triangles;
    std::unordered_map<std::uint64_t,std::vector<std::uint32_t>> bins;
    std::uint64_t entries = 0;
    for (std::size_t row = 0; row+1 < shape.rows; ++row)
        for (std::size_t col = 0; col+1 < shape.cols; ++col) {
            const auto a = std::uint32_t(row*shape.cols+col),b = a+1,c = a+std::uint32_t(shape.cols),d = c+1;
            for (const Triangle triangle : {Triangle{a,b,d},Triangle{a,d,c}}) {
                if (std::ranges::any_of(triangle,[&](auto i) { return !source.valid.values()[i]; })) continue;
                const auto& p = points[triangle[0]]; const auto& q = points[triangle[1]]; const auto& r = points[triangle[2]];
                const auto edge = std::max({distance(p,q),distance(q,r),distance(r,p)});
                if (edge > std::min(.5,typical*8) || std::abs(dot(p,cross(q,r))) < 1e-14) continue;
                const auto index = std::uint32_t(triangles.size());
                triangles.push_back(triangle);
                /// @note 球面弧相对弦的偏离也纳入包围盒。
                const auto margin = edge*edge;
                int lo[3],hi[3];
                for (int k = 0; k < 3; ++k) {
                    lo[k] = bin(std::min({p[k],q[k],r[k]})-margin);
                    hi[k] = bin(std::max({p[k],q[k],r[k]})+margin);
                }
                for (int x = lo[0]; x <= hi[0]; ++x)
                    for (int y = lo[1]; y <= hi[1]; ++y)
                        for (int z = lo[2]; z <= hi[2]; ++z) { bins[key(x,y,z)].push_back(index); ++entries; }
            }
        }
    workspace.resize(points.size()*sizeof(Vector)+triangles.capacity()*sizeof(Triangle)+
                     entries*sizeof(std::uint32_t)+bins.size()*64);
    const auto& lookup = bins;
    context.parallel_rows(output.rows,[&](std::size_t begin,std::size_t end) {
        for (auto i = begin*output.cols; i < end*output.cols; ++i) {
            if (!target.valid.values()[i]) continue;
            const auto p = position(target.latitude.values()[i],target.longitude.values()[i]);
            const auto found = lookup.find(key(bin(p[0]),bin(p[1]),bin(p[2])));
            if (found == lookup.end()) continue;
            for (auto t : found->second) {
                const auto indices = triangles[t];
                const auto& a = points[indices[0]]; const auto& b = points[indices[1]]; const auto& c = points[indices[2]];
                const auto determinant = dot(a,cross(b,c));
                Vector weights{dot(p,cross(b,c))/determinant,dot(p,cross(c,a))/determinant,dot(p,cross(a,b))/determinant};
                const auto total = weights[0]+weights[1]+weights[2];
                if (total <= 0) continue;
                for (auto& w : weights) w /= total;
                if (std::ranges::any_of(weights,[](double w) { return w < -1e-6 || w > 1+1e-6; })) continue;
                auto& entry = result.values()[i]; entry.indices = indices;
                double sum = 0;
                for (int k = 0; k < 3; ++k) { weights[k] = std::clamp(weights[k],0.,1.); sum += weights[k]; }
                for (int k = 0; k < 3; ++k) entry.weights[k] = float(weights[k]/sum);
                break;
            }
        }
    });
    return result;
}

ScalarPlane resample_geographic(const ScalarPlane& source,const Raster<GeographicWeights>& weights,
                               ExecutionContext& context) {
    if (source.values.shape() != source.valid.shape()) throw std::invalid_argument("Geographic source mask mismatch");
    ScalarPlane result{source.unit,Raster<float>(weights.shape()),Raster<std::uint8_t>(weights.shape())};
    auto phase = context.profile().phase("geographic_resample");
    context.parallel_rows(weights.shape().rows,[&](std::size_t begin,std::size_t end) {
        for (auto i = begin*weights.shape().cols; i < end*weights.shape().cols; ++i) {
            const auto& entry = weights.values()[i];
            double value = 0,total = 0;
            bool good = true;
            for (int k = 0; k < 3; ++k) {
                if (entry.weights[k] <= 0) continue;
                const auto j = entry.indices[k];
                if (j >= source.values.values().size()) throw std::invalid_argument("Geographic source index out of bounds");
                good = good && source.valid.values()[j] && std::isfinite(source.values.values()[j]);
                total += entry.weights[k]; value += entry.weights[k]*double(source.values.values()[j]);
            }
            result.valid.values()[i] = good && total > .99;
            result.values.values()[i] = result.valid.values()[i] ? float(value/total) : NAN;
        }
    });
    context.profile().record_computed(weights.shape().elements());
    return result;
}

}
