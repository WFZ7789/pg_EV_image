/// SPDX-License-Identifier: MIT

#pragma once

#include "core/execution.hpp"

#include <algorithm>
#include <cmath>

namespace fy::products::detail {

using namespace core;

struct Scan { std::size_t begin,end; };
struct RowRange { std::size_t first,last; };

/// @brief 独立反解各扫描的双线性定位网格，不把相邻条带行号视作相邻地理位置。
class ScanTemperatureSampler {
public:
    ScanTemperatureSampler(const ScalarPlane& temperature,const SwathGeometry& geometry)
        : temperature_(temperature),geometry_(geometry),shape_(geometry.latitude.shape()) {
        geometry.validate();
        if (shape_ != temperature.values.shape() || shape_ != temperature.valid.shape())
            throw std::invalid_argument("Scan temperature grid mismatch");
        const auto scan_rows = geometry.scan_rows ? geometry.scan_rows : shape_.rows;
        for (std::size_t row = 0; row < shape_.rows;) {
            const auto end = std::min(shape_.rows,row+scan_rows-(geometry.first_source_row+row)%scan_rows);
            scans_.push_back({row,end});
            for (std::size_t col = 0; col < shape_.cols; ++col) {
                RowRange range{end,row};
                for (auto r = row; r < end; ++r) {
                    const auto i = r*shape_.cols+col;
                    if (geometry.valid.values()[i] && temperature.valid.values()[i] && std::isfinite(temperature.values.values()[i])) {
                        range.first = std::min(range.first,r); range.last = r;
                    }
                }
                ranges_.push_back(range);
            }
            row = end;
        }
    }

    std::size_t bytes() const { return scans_.capacity()*sizeof(Scan)+ranges_.capacity()*sizeof(RowRange); }

    std::optional<float> offset(std::size_t row,std::size_t col,int dy,int dx) const {
        const auto scan_rows = geometry_.scan_rows ? geometry_.scan_rows : shape_.rows;
        const auto scan = (geometry_.first_source_row%scan_rows+row)/scan_rows;
        const auto [begin,end] = scans_[scan];
        const auto target_row = std::int64_t(row)+dy,target_col = std::int64_t(col)+dx;
        if (target_col < 0 || target_col >= std::int64_t(shape_.cols)) return {};
        if (target_row >= std::int64_t(begin) && target_row < std::int64_t(end)) {
            const auto i = std::size_t(target_row)*shape_.cols+std::size_t(target_col);
            if (geometry_.valid.values()[i] && temperature_.valid.values()[i] && std::isfinite(temperature_.values.values()[i]))
                return temperature_.values.values()[i];
        }
        if (end-begin < 2) return {};
        /// @note 越界行号沿原扫描的定位网格外推，不能直接使用下一扫描的同一行列偏移。
        const auto r = std::size_t(std::clamp(target_row,std::int64_t(begin),std::int64_t(end-2)));
        const auto i = r*shape_.cols+std::size_t(target_col),j = i+shape_.cols;
        if (!geometry_.valid.values()[i] || !geometry_.valid.values()[j]) return {};
        const auto u = double(target_row)-r;
        const auto latitude = std::lerp(double(geometry_.latitude.values()[i]),double(geometry_.latitude.values()[j]),u);
        const auto longitude = double(geometry_.longitude.values()[i])+u*
            std::remainder(double(geometry_.longitude.values()[j])-geometry_.longitude.values()[i],360.);
        for (bool edge : {false,true}) {
            if (auto value = in_scan(latitude,longitude,row,col,scan,edge)) return value;
            for (std::size_t delta = 1; delta <= 8; ++delta) {
                if (scan >= delta)
                    if (auto value = in_scan(latitude,longitude,row,col,scan-delta,edge)) return value;
                if (scan+delta < scans_.size())
                    if (auto value = in_scan(latitude,longitude,row,col,scan+delta,edge)) return value;
            }
        }
        return {};
    }

private:
    const ScalarPlane& temperature_;
    const SwathGeometry& geometry_;
    Shape shape_;
    std::vector<Scan> scans_;
    std::vector<RowRange> ranges_;

    std::optional<float> in_scan(double latitude,double longitude,std::size_t source_row,std::size_t source_col,
                                 std::size_t scan,bool edge) const {
        const auto [begin,end] = scans_[scan];
        if (end-begin < 2 || shape_.cols < 2) return {};
        double row = std::clamp(double(source_row),double(begin),double(end-1)),col = source_col;
        const auto origin = geometry_.longitude.view().at(std::size_t(row),source_col);
        const auto target_lon = std::remainder(longitude-origin,360.);
        bool converged = false;
        for (int iteration = 0; iteration < 6; ++iteration) {
            const auto r = std::size_t(std::clamp(std::floor(row),double(begin),double(end-2)));
            const auto c = std::size_t(std::clamp(std::floor(col),0.,double(shape_.cols-2)));
            const std::array<std::size_t,4> indices{r*shape_.cols+c,r*shape_.cols+c+1,(r+1)*shape_.cols+c,(r+1)*shape_.cols+c+1};
            std::array<double,4> lat,lon;
            for (std::size_t k = 0; k < 4; ++k) {
                if (!geometry_.valid.values()[indices[k]]) return {};
                lat[k] = geometry_.latitude.values()[indices[k]];
                lon[k] = std::remainder(double(geometry_.longitude.values()[indices[k]])-origin,360.);
            }
            const auto u = row-r,v = col-c;
            const auto interpolate = [u,v](const auto& p) { return std::lerp(std::lerp(p[0],p[1],v),std::lerp(p[2],p[3],v),u); };
            const auto lat_row = std::lerp(lat[2]-lat[0],lat[3]-lat[1],v),lat_col = std::lerp(lat[1]-lat[0],lat[3]-lat[2],u);
            const auto lon_row = std::lerp(lon[2]-lon[0],lon[3]-lon[1],v),lon_col = std::lerp(lon[1]-lon[0],lon[3]-lon[2],u);
            const auto determinant = lat_row*lon_col-lat_col*lon_row;
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-14) return {};
            const auto dy = latitude-interpolate(lat),dx = target_lon-interpolate(lon);
            const auto dr = (dy*lon_col-dx*lat_col)/determinant,dc = (dx*lat_row-dy*lon_row)/determinant;
            row += dr; col += dc;
            if (!std::isfinite(row) || !std::isfinite(col) || row < double(begin)-.5 || row > double(end)-.5 ||
                col < -.5 || col > double(shape_.cols)-.5) return {};
            if (std::abs(dr) < 1e-4 && std::abs(dc) < 1e-4) { converged = true; break; }
        }
        if (!converged) return {};
        col = std::clamp(col,0.,double(shape_.cols-1));
        const auto c = std::min(std::size_t(col),shape_.cols-2);
        const auto a = ranges_[scan*shape_.cols+c],b = ranges_[scan*shape_.cols+c+1];
        const auto first = std::max(a.first,b.first),last = std::min(a.last,b.last);
        const auto margin = edge ? .5 : 1e-4;
        if (first > last || row < double(first)-margin || row > double(last)+margin) return {};
        row = std::clamp(row,double(first),double(last));
        const auto r = std::min(std::size_t(row),end-2);
        const auto u = row-r,v = col-c;
        const std::array<std::size_t,4> indices{r*shape_.cols+c,r*shape_.cols+c+1,(r+1)*shape_.cols+c,(r+1)*shape_.cols+c+1};
        const std::array<double,4> weights{(1-u)*(1-v),(1-u)*v,u*(1-v),u*v};
        double value = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            if (weights[k] <= 1e-10) continue;
            const auto i = indices[k];
            if (!temperature_.valid.values()[i] || !std::isfinite(temperature_.values.values()[i])) return {};
            value += weights[k]*temperature_.values.values()[i];
        }
        return float(value);
    }
};

}
