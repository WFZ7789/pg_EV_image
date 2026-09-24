#ifndef PG_EV_IMAGE_MODES_PMR_MWRI_H
#define PG_EV_IMAGE_MODES_PMR_MWRI_H

#include "stl.h"
#include "phys.h"
#include "color.h"
#include "read.h"
#include "rend.h"
#include "context.h"

using namespace std;
using namespace H5;

/// @brief  降水产品渲染模式
inline void mode_PMR(Context& ctx) {
    DataSet latitude = ctx.geofile.openDataSet("Geo_Fields/Latitude");
    DataSet longitude = ctx.geofile.openDataSet("Geo_Fields/Longitude");
    auto geo_dims = read_dims(latitude);
    auto read_geo = [&] (const DataSet& src) {
        auto raw = rer_f32_pst_bdid_base(src, 0).first;
        vector<double> res(raw.size());
        copy(raw.begin(), raw.end(), res.begin());
        return res;
    };
    auto lat = read_geo(latitude), lon = read_geo(longitude);
    for (auto& i : lon)
        if (i < 0)
            i += 360;
    cerr << *min_element(lat.begin(), lat.end()) << endl;
    DataSet zfactor = ctx.file.openDataSet("SLV/zFactorCorrectedESurface");
    auto mat = rer2D_f32_base(zfactor);
	hsize_t txmin = geo_dims.first[0], txmax = 0,
			tymin = geo_dims.first[1], tymax = 0;
	for (int i = 0; i < lat.size(); i++)
		if (lat[i] >= ctx.target_lat - ctx.target_square_size &&
		    lat[i] <= ctx.target_lat + ctx.target_square_size &&
		    lon[i] >= ctx.target_lon - ctx.target_square_size &&
		    lon[i] <= ctx.target_lon + ctx.target_square_size)
		{
			auto tx = i / geo_dims.first[1],
					ty = i % geo_dims.first[1];
			txmin = min(txmin, tx);
			txmax = max(txmax, tx);
			tymin = min(tymin, ty);
			tymax = max(tymax, ty);
		}
	if (txmax < txmin || tymax < tymin)
		throw invalid_argument("目标区域不在数据覆盖范围内: " + to_string(ctx.target_lat) + ", " +
		                       to_string(ctx.target_lon));
	vector<float> target_mat, target_lat_mat, target_lon_mat;
    auto totsize = (txmax - txmin + 1) *
                   (tymax - tymin + 1);
    target_mat.reserve(totsize);
    target_lat_mat.reserve(totsize);
    target_lon_mat.reserve(totsize);
    for (int i = txmin; i <= txmax; i++)
        for (int j = tymin; j <= tymax; j++)
        {
            int id = i * geo_dims.first[1] + j;
            target_mat.push_back(mat.first[id]);
            target_lat_mat.push_back(lat[id]);
            target_lon_mat.push_back(lon[id]);
        }
    vector<hsize_t> target_dims{0, txmax - txmin + 1, tymax - tymin + 1};
    Group g = ctx.file.openGroup("/");
    vector lbls = {
        read_str_attr(g.openAttribute("File Name")),
        read_str_attr(g.openAttribute("Observing Beginning Date")),
        read_str_attr(g.openAttribute("Observing Beginning Time")) + "(UTC)",
        ctx.sat + " " + read_str_attr(g.openAttribute("File Alias Name")),
        read_str_attr(zfactor.openAttribute("Long_Name")),
    };
    rend(target_dims, target_mat, target_lat_mat, target_lon_mat, ctx.out_name, lbls,
         "", color_ir_ca, {0.0f, *ranges::max_element(target_mat)});
}

struct MwriTargetArea {
    array<vector<float>, 3> values; // 亮温、纬度、经度
    vector<hsize_t> dims;            // 行、列
};

// FY-3D 与 FY-3H MWRI-II 的 Window Channel 0~9 共用这一组顺序。
inline constexpr int MWRI_WINDOW_10V = 0;
inline constexpr int MWRI_WINDOW_10H = 1;
inline constexpr int MWRI_WINDOW_18V = 2;
inline constexpr int MWRI_WINDOW_18H = 3;
inline constexpr int MWRI_WINDOW_23V = 4;
inline constexpr int MWRI_WINDOW_23H = 5;
inline constexpr int MWRI_WINDOW_36V = 6;
inline constexpr int MWRI_WINDOW_36H = 7;
inline constexpr int MWRI_WINDOW_89V = 8;
inline constexpr int MWRI_WINDOW_89H = 9;

// 这些编号只属于 FY-3H MWRI-II 的 Sounding Channel，不能用于 FY-3D。
inline constexpr int MWRI_SOUNDING_5375V = 6;
inline constexpr int MWRI_SOUNDING_118_P32 = 8;
inline constexpr int MWRI_SOUNDING_118_P14 = 10;
inline constexpr int MWRI_SOUNDING_118_P12 = 11;

inline double normalize_mwri_lon(double lon) {
    if (!isfinite(lon))
        return NAN;
    double res = fmod(lon, 360.0);
    if (res < 0)
        res += 360.0;
    return res == 360.0 ? 0.0 : res;
}

/// @brief 计算经度相对目标中心的最短有向差，避免 0/360 度边界裁剪失败
inline double mwri_lon_delta(double lon, double target_lon) {
    return remainder(normalize_mwri_lon(lon) - normalize_mwri_lon(target_lon), 360.0);
}

/// @brief 按目标经纬度裁剪 MWRI 扫描带的矩形包围盒
inline MwriTargetArea crop_mwri_target(const vector<float>& data, const vector<hsize_t>& data_dims,
                                        const vector<float>& lat, const vector<float>& lon,
                                        double target_lat, double target_lon, double half) {
    if (data_dims.size() != 2 || lat.size() != lon.size() ||
        lat.size() != static_cast<size_t>(data_dims[0] * data_dims[1]) ||
        data.size() != lat.size())
        throw invalid_argument("MWRI 亮温与地理定位数据维度不一致");
    if (!(half > 0) || !isfinite(target_lat) || !isfinite(target_lon))
        throw invalid_argument("MWRI 目标区域参数无效");

    const hsize_t rows = data_dims[0], cols = data_dims[1];
    const double center_lon = normalize_mwri_lon(target_lon);
    hsize_t r0 = rows, r1 = 0, c0 = cols, c1 = 0;
    bool found = false;
    for (size_t i = 0; i < lat.size(); ++i) {
        if (!isfinite(lat[i]) || !isfinite(lon[i]))
            continue;
        const double dlon = mwri_lon_delta(lon[i], center_lon);
        if (lat[i] >= target_lat - half && lat[i] <= target_lat + half &&
            abs(dlon) <= half) {
            const hsize_t row = i / cols, col = i % cols;
            r0 = min(r0, row);
            r1 = max(r1, row);
            c0 = min(c0, col);
            c1 = max(c1, col);
            found = true;
        }
    }
    if (!found)
        throw invalid_argument("目标区域不在 MWRI 数据覆盖范围内: " + to_string(target_lat) + ", " +
                               to_string(target_lon));

    const hsize_t target_rows = r1 - r0 + 1, target_cols = c1 - c0 + 1;
    const size_t target_size = static_cast<size_t>(target_rows * target_cols);
    MwriTargetArea result;
    result.dims = {target_rows, target_cols};
    for (auto& field : result.values)
        field.resize(target_size);

    for (hsize_t row = r0; row <= r1; ++row)
        for (hsize_t col = c0; col <= c1; ++col) {
            const size_t source_id = static_cast<size_t>(row * cols + col);
            const size_t target_id = static_cast<size_t>((row - r0) * target_cols + col - c0);
            result.values[0][target_id] = data[source_id];
            result.values[1][target_id] = lat[source_id];
            result.values[2][target_id] =
                isfinite(lon[source_id]) ? center_lon + mwri_lon_delta(lon[source_id], center_lon) : NAN;
        }
    return result;
}

/// @brief 将裁剪后的 MWRI 经纬度转换为以目标中心为原点的正交投影坐标
inline void project_mwri_orthographic(vector<float>& lat, vector<float>& lon,
                                      double target_lat, double target_lon) {
    const double center_lat = target_lat * Deg2rad;
    const double center_lon = normalize_mwri_lon(target_lon) * Deg2rad;
    for (size_t i = 0; i < lat.size(); ++i) {
        if (!isfinite(lat[i]) || !isfinite(lon[i])) {
            lat[i] = lon[i] = NAN;
            continue;
        }
        const double point_lat = lat[i] * Deg2rad;
        const double delta_lon = mwri_lon_delta(lon[i], target_lon) * Deg2rad;
        const double projected_x = cos(point_lat) * sin(delta_lon);
        const double projected_y = cos(center_lat) * sin(point_lat) -
                                   sin(center_lat) * cos(point_lat) * cos(delta_lon);
        lon[i] = static_cast<float>(projected_x / Deg2rad);
        lat[i] = static_cast<float>(projected_y / Deg2rad);
    }
}

inline pair<float, float> finite_minmax(const vector<float>& values) {
    float low = numeric_limits<float>::max(), high = numeric_limits<float>::lowest();
    for (float value : values)
        if (isfinite(value)) {
            low = min(low, value);
            high = max(high, value);
        }
    if (low == numeric_limits<float>::max())
        throw invalid_argument("MWRI 目标区域没有有效亮温数据");
    return {low, high == low ? low + 1.0f : high};
}

inline void mode_MWRI(Context& ctx, const string& mode) {
    if (ctx.sat == "FY-3D" && mode == "S")
        throw invalid_argument(string{"FY-3D MWRI 没有 FY-3H MWRI-II 的 Sounding Channel，请使用 -m W，"} +
                               "并按 FY-3D 的 0~9 通道编号选择窗区通道");
    if (ctx.sat == "FY-3D" && (mode == "TCN" || mode == "TCN2"))
        throw invalid_argument("FY-3D MWRI 仅有 10 个窗区通道，不含 FY-3H MWRI-II 的 118 GHz 探测通道，" +
                               mode + " 不能用于 FY-3D");

    DataSet latitude, longitude, mwri_bt;
    auto read = [&] (const string& channel_type, int band_id)
        -> pair<array<vector<float>, 3>, vector<hsize_t>> {
        if (ctx.sat == "FY-3D") {
            latitude = ctx.geofile.openDataSet("Geolocation/Latitude");
            longitude = ctx.geofile.openDataSet("Geolocation/Longitude");
            auto [lat, geo_dims] = read_geolocation(latitude);
            auto lon = read_geolocation(longitude).first;
            if (lat.size() != lon.size())
                throw invalid_argument("FY-3D MWRI 经纬度数据维度不一致");
            for (size_t i = 0; i < lat.size(); ++i) {
                if (!isfinite(lat[i]) || lat[i] < -90.0f || lat[i] > 90.0f)
                    lat[i] = NAN;
                if (!isfinite(lon[i]) || lon[i] < -180.0f || lon[i] > 360.0f)
                    lon[i] = NAN;
            }
            mwri_bt = ctx.file.openDataSet("Calibration/EARTH_OBSERVE_BT_10_to_89GHz");
            // FY-3D 的 0~9 与 FY-3H 的 Window Channel 0~9 一致，
            // 但 FY-3H 的 Sounding Channel 0~11 是另一套 50/53/118 GHz 定义。
            auto [data, data_dims] = read_mwri_fy3d_channel(mwri_bt, band_id);
            if (data_dims != geo_dims)
                throw invalid_argument("FY-3D MWRI 亮温与地理定位数据维度不一致");
            auto target = crop_mwri_target(data, data_dims, lat, lon,
                                           ctx.target_lat, ctx.target_lon, ctx.target_square_size);
            if (ctx.aq)
                project_mwri_orthographic(target.values[1], target.values[2],
                                          ctx.target_lat, ctx.target_lon);
            return {std::move(target.values), std::move(target.dims)};
        }

        latitude = ctx.geofile.openDataSet(channel_type + "/Geolocation/Latitude");
        longitude = ctx.geofile.openDataSet(channel_type + "/Geolocation/Longitude");
        auto [lat, gdims] = read_geolocation(latitude);
        auto lon = read_geolocation(longitude).first;
        mwri_bt = ctx.file.openDataSet(channel_type + "/Calibration/EARTH_OBSERVE_BT");
        auto [data, dims] = rer_f32_pst_bdid_base(mwri_bt, band_id);
		hsize_t txmin = gdims[0], txmax = 0,
				tymin = gdims[1], tymax = 0;
		for (int i = 0; i < lat.size(); i++)
			if (lat[i] >= ctx.target_lat - ctx.target_square_size &&
			    lat[i] <= ctx.target_lat + ctx.target_square_size &&
			    lon[i] >= ctx.target_lon - ctx.target_square_size &&
			    lon[i] <= ctx.target_lon + ctx.target_square_size)
			{
				auto tx = i / gdims[1],
						ty = i % gdims[1];
				txmin = min(txmin, tx);
				txmax = max(txmax, tx);
				tymin = min(tymin, ty);
				tymax = max(tymax, ty);
			}
		if (txmax < txmin || tymax < tymin)
			throw invalid_argument("目标区域不在数据覆盖范围内: " + to_string(ctx.target_lat) + ", " +
			                       to_string(ctx.target_lon));
		vector<float> target_mat, target_lat_mat, target_lon_mat;
        auto totsize = (txmax - txmin + 1) *
                       (tymax - tymin + 1);
        target_mat.reserve(totsize);
        target_lat_mat.reserve(totsize);
        target_lon_mat.reserve(totsize);
        for (int i = txmin; i <= txmax; i++)
            for (int j = tymin; j <= tymax; j++)
            {
                int id = i * gdims[1] + j;
                target_mat.push_back(data[id]);
                target_lat_mat.push_back(lat[id]);
                target_lon_mat.push_back(lon[id]);
            }
        if (ctx.aq)
            project_mwri_orthographic(target_lat_mat, target_lon_mat,
                                      ctx.target_lat, ctx.target_lon);
        vector target_dims{txmax - txmin + 1, tymax - tymin + 1};
        return {{target_mat, target_lat_mat, target_lon_mat}, target_dims};
    };
    if (mode == "S" || mode == "W")
    {
        string channel{mode == "S" ? "Sounding Channel" : "Window Channel"};
        auto [target_area, target_dims] = read(channel, ctx.band_id);
        auto [min_bt, max_bt] = finite_minmax(target_area[0]);
        static const array<string, 10> fy3d_channel_names{
            "10.65V", "10.65H", "18.7V", "18.7H", "23.8V",
            "23.8H", "36.5V", "36.5H", "89V", "89H"
        };
        const string channel_name = ctx.sat == "FY-3D" && ctx.band_id >= 0 && ctx.band_id < 10
                                        ? " (" + fy3d_channel_names[ctx.band_id] + ")" : "";
        vector lbls = {
            read_str_attr(ctx.file.openAttribute("File Name")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Date")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Time")) + "(UTC)",
            ctx.sat + " " + ctx.sen_code,
            read_str_attr(mwri_bt.openAttribute("long_name")) + " #" + to_string(ctx.band_id) + channel_name,
        };
        target_dims.insert(target_dims.begin(), 0);
        rend(target_dims, target_area[0], target_area[1], target_area[2], ctx.out_name, lbls,
             "", color_plasma, {min_bt, max_bt});
    }
    else if (mode == "W84")
    {
        string channel{"Window Channel"};
        auto [target_area_89V, target_dims] = read(channel, MWRI_WINDOW_89V);
        auto mat_89H = read(channel, MWRI_WINDOW_89H).first[0];
        auto mat_37H = read(channel, MWRI_WINDOW_36H).first[0];
        hsize_t N = mat_37H.size();
        vector<cv::Vec3b> comp(N);
        for (int i = 0; i < N; i++)
        {
            float pct = 1.818 * target_area_89V[0][i] - 0.818 * mat_89H[i];
            //float resb = (300 - mat_89H[i]) * 1.5,
            //      resg = (mat_37H[i] - 120) * 255 / 180,
            //      resr = (300 - target_area_89V[0][i]) * 1.6;
            float resb = (300 - mat_89H[i]) * 1.5,
                    resg = (300 - target_area_89V[0][i]) * 1.5,
                    resr = (300 - pct) * 1.5;

            comp[i] = {
                static_cast<uint8_t>(clamp<int>(resb, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resg, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resr, 0, 255))
            };
        }
        vector lbls = {
            read_str_attr(ctx.file.openAttribute("File Name")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Date")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Time")) + "(UTC)",
            ctx.sat + " " + ctx.sen_code,
            read_str_attr(mwri_bt.openAttribute("long_name")),
        };
        rend_cl(comp, target_area_89V[1], target_area_89V[2], ctx.out_name,
                lbls, "", target_dims[0], target_dims[1]);
    }
        else if (mode == "TCX")
    {
        /// TC 结构增强 (W84 升级): R=PCT89 G=极化差(89V-89H) B=散射差(89V-36V)
        string channel{"Window Channel"};
        auto [target_area_89V, target_dims] = read(channel, MWRI_WINDOW_89V);
        auto mat_89H = read(channel, MWRI_WINDOW_89H).first[0];
        auto mat_36V = read(channel, MWRI_WINDOW_36V).first[0];
        hsize_t N = mat_36V.size();
        vector<cv::Vec3b> comp(N);
        for (int i = 0; i < N; i++)
        {
            float pct = 1.818 * target_area_89V[0][i] - 0.818 * mat_89H[i],
                  pol = target_area_89V[0][i] - mat_89H[i],
                  scat = target_area_89V[0][i] - mat_36V[i];
            float resr = (300 - pct) * 1.5,
                  resg = pol * 5,
                  resb = (scat + 80) * 1.8;
            comp[i] = {
                static_cast<uint8_t>(clamp<int>(resb, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resg, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resr, 0, 255))
            };
        }
        vector<string> lbls = {
            read_str_attr(ctx.file.openAttribute("File Name")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Date")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Time")) + "(UTC)",
            ctx.sat + " " + ctx.sen_code,
            "TCX: PCT89 / Pol / Scat",
        };
        rend_cl(comp, target_area_89V[1], target_area_89V[2], ctx.out_name,
                lbls, "", target_dims[0], target_dims[1]);
    }
    else if (mode == "TCN")
    {
        /// TC 暖心: 118GHz 三层温度 R=118-4(高层) G=118-3(中层) B=118-1(低层)
        string channel{"Sounding Channel"};
        auto [target_area_4, target_dims] = read(channel, MWRI_SOUNDING_118_P12);
        auto mat_3 = read(channel, MWRI_SOUNDING_118_P14).first[0];
        auto mat_1 = read(channel, MWRI_SOUNDING_118_P32).first[0];
        hsize_t N = mat_1.size();
        vector<cv::Vec3b> comp(N);
        for (int i = 0; i < N; i++)
        {
            float resr = (target_area_4[0][i] - 200) * 3,
                  resg = (mat_3[i] - 200) * 3,
                  resb = (mat_1[i] - 240) * 3;
            comp[i] = {
                static_cast<uint8_t>(clamp<int>(resb, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resg, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resr, 0, 255))
            };
        }
        vector<string> lbls = {
            read_str_attr(ctx.file.openAttribute("File Name")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Date")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Time")) + "(UTC)",
            ctx.sat + " " + ctx.sen_code,
            "TCN: 118GHz Warm Core",
        };
        rend_cl(comp, target_area_4[1], target_area_4[2], ctx.out_name,
                lbls, "", target_dims[0], target_dims[1]);
    }
    else if (mode == "TCS")
    {
        /// TC 冰散射: 频率差消表面信号 R=89V-36V G=36V-18V B=23V-18V
        string channel{"Window Channel"};
        auto [target_area_89V, target_dims] = read(channel, MWRI_WINDOW_89V);
        auto mat_36V = read(channel, MWRI_WINDOW_36V).first[0];
        auto mat_23V = read(channel, MWRI_WINDOW_23V).first[0];
        auto mat_18V = read(channel, MWRI_WINDOW_18V).first[0];
        hsize_t N = mat_18V.size();
        vector<cv::Vec3b> comp(N);
        for (int i = 0; i < N; i++)
        {
            float s89 = target_area_89V[0][i] - mat_36V[i],
                  s36 = mat_36V[i] - mat_18V[i],
                  s23 = mat_23V[i] - mat_18V[i];
            float resr = (s89 + 50) * 2.5,
                  resg = (s36 + 17) * 5,
                  resb = (s23 + 17) * 5;
            comp[i] = {
                static_cast<uint8_t>(clamp<int>(resb, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resg, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resr, 0, 255))
            };
        }
        vector<string> lbls = {
            read_str_attr(ctx.file.openAttribute("File Name")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Date")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Time")) + "(UTC)",
            ctx.sat + " " + ctx.sen_code,
            "TCS: 89-36 / 36-18 / 23-18",
        };
        rend_cl(comp, target_area_89V[1], target_area_89V[2], ctx.out_name,
                lbls, "", target_dims[0], target_dims[1]);
    }
    else if (mode == "TCR")
    {
        /// TC 降水: R=PCT89 G=23V(水汽) B=36V(云水)
        string channel{"Window Channel"};
        auto [target_area_89V, target_dims] = read(channel, MWRI_WINDOW_89V);
        auto mat_89H = read(channel, MWRI_WINDOW_89H).first[0];
        auto mat_23V = read(channel, MWRI_WINDOW_23V).first[0];
        auto mat_36V = read(channel, MWRI_WINDOW_36V).first[0];
        hsize_t N = mat_36V.size();
        vector<cv::Vec3b> comp(N);
        for (int i = 0; i < N; i++)
        {
            float pct = 1.818 * target_area_89V[0][i] - 0.818 * mat_89H[i];
            float resr = (300 - pct) * 1.5,
                  resg = (300 - mat_23V[i]) * 1.5,
                  resb = (300 - mat_36V[i]) * 1.5;
            comp[i] = {
                static_cast<uint8_t>(clamp<int>(resb, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resg, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resr, 0, 255))
            };
        }
        vector<string> lbls = {
            read_str_attr(ctx.file.openAttribute("File Name")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Date")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Time")) + "(UTC)",
            ctx.sat + " " + ctx.sen_code,
            "TCR: PCT89 / 23V / 36V",
        };
        rend_cl(comp, target_area_89V[1], target_area_89V[2], ctx.out_name,
                lbls, "", target_dims[0], target_dims[1]);
    }
    else if (mode == "TCN2")
    {
        /// TC 暖心增强 (54+118): R=53.75V(高层~10km) G=118±1.2(中层~6.6km) B=118±3.2(低层~0km)
        string channel{"Sounding Channel"};
        auto [target_area_54, target_dims] = read(channel, MWRI_SOUNDING_5375V);
        auto mat_118h = read(channel, MWRI_SOUNDING_118_P12).first[0];
        auto mat_118l = read(channel, MWRI_SOUNDING_118_P32).first[0];
        hsize_t N = mat_118l.size();
        vector<cv::Vec3b> comp(N);
        for (int i = 0; i < N; i++)
        {
            float resr = (target_area_54[0][i] - 210) * 4,
                  resg = (mat_118h[i] - 210) * 4,
                  resb = (mat_118l[i] - 240) * 3;
            comp[i] = {
                static_cast<uint8_t>(clamp<int>(resb, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resg, 0, 255)),
                static_cast<uint8_t>(clamp<int>(resr, 0, 255))
            };
        }
        vector<string> lbls = {
            read_str_attr(ctx.file.openAttribute("File Name")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Date")),
            read_str_attr(ctx.file.openAttribute("Observing Beginning Time")) + "(UTC)",
            ctx.sat + " " + ctx.sen_code,
            "TCN2: 53.75V / 118±1.2 / 118±3.2",
        };
        rend_cl(comp, target_area_54[1], target_area_54[2], ctx.out_name,
                lbls, "", target_dims[0], target_dims[1]);
    }
    else
        throw invalid_argument("不支持的 MWRI 模式: " + mode);
}

#endif
