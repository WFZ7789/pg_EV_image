#ifndef PG_EV_IMAGE_READ_H
#define PG_EV_IMAGE_READ_H

#include    "stl.h"
#include    "phys.h"

using namespace std;
using namespace H5;

/// @brief  读取数据集维度信息
pair<vector<hsize_t>, DataSpace> read_dims(const auto& src) {
    auto space = src.getSpace();
    int dimn_rank = space.getSimpleExtentNdims();
    vector<hsize_t> dims(dimn_rank);
    space.getSimpleExtentDims(dims.data());
    return {dims, space};
}

/// @brief  读取    3D  数据集指定波段  (UINT16 ->  float   定标)
inline pair<vector<float>, vector<hsize_t>> rer_base(const DataSet& src, hsize_t bdid) {
    auto dims = read_dims(src);
    assert(bdid < dims.first[0]);
    hsize_t start[3]{bdid, 0, 0}, count[3]{1, dims.first[1], dims.first[2]};
    DataSpace rd = src.getSpace(), ms(3, count);
    rd.selectHyperslab(H5S_SELECT_SET, count, start);
    vector<float> res(static_cast<size_t>(count[1] * count[2]));
    src.read(res.data(), PredType::NATIVE_FLOAT, ms, rd);
    Attribute slope_att = src.openAttribute("Slope"), itcpt_att = src.openAttribute("Intercept");
    vector<float> slope(dims.first[0]), intercept(dims.first[0]);
    slope_att.read(PredType::NATIVE_FLOAT, slope.data());
    itcpt_att.read(PredType::NATIVE_FLOAT, intercept.data());
    if (slope_att.getSpace().getSimpleExtentNpoints() <= bdid)
        bdid = 0;
    for (auto& value : res)
        value = value * slope[bdid] + intercept[bdid];
    return {res, dims.first};
}

/// @brief  读取    3D  数据集指定波段  (FLOAT  ->  float   定标)
inline pair<vector<float>, vector<hsize_t>> rer_f32_base(const DataSet& src, hsize_t bdid) {
    auto dims = read_dims(src);
    assert(bdid < dims.first[0]);
    hsize_t start[3]{bdid, 0, 0}, count[3]{1, dims.first[1], dims.first[2]};
    DataSpace rd = src.getSpace(), ms(3, count);
    rd.selectHyperslab(H5S_SELECT_SET, count, start);
    vector<float> res(static_cast<size_t>(count[1] * count[2]));
    src.read(res.data(), PredType::NATIVE_FLOAT, ms, rd);
    Attribute slope_att = src.openAttribute("Slope"), itcpt_att = src.openAttribute("Intercept");
    int acpc = slope_att.getSpace().getSimpleExtentNpoints() == 1 ? 0 : bdid;
    vector<float> slope(dims.first[0]), intercept(dims.first[0]);
    slope_att.read(PredType::NATIVE_FLOAT, slope.data());
    itcpt_att.read(PredType::NATIVE_FLOAT, intercept.data());
    for (auto& value : res)
        value = value * slope[acpc] + intercept[acpc];
    return {res, dims.first};
}

/// @brief 读取 3D 数据集指定波段 -> float 定标)
inline pair<vector<float>, vector<hsize_t>> rer_f32_pst_bdid_base(const DataSet& src, hsize_t bdid) {
    auto dims = read_dims(src);
    assert(bdid < dims.first[2]);
    hsize_t start[3]{0, 0, bdid}, count[3]{dims.first[0], dims.first[1], 1};
    DataSpace rd = src.getSpace(), ms(3, count);
    rd.selectHyperslab(H5S_SELECT_SET, count, start);
    vector<float> res(static_cast<size_t>(count[0] * count[1]));
    src.read(res.data(), PredType::NATIVE_FLOAT, ms, rd);
    Attribute slope_att = src.openAttribute("Slope"), itcpt_att = src.openAttribute("Intercept");
    int acpc = slope_att.getSpace().getSimpleExtentNpoints() == 1 ? 0 : bdid;
    vector<float> slope(dims.first[0]), intercept(dims.first[0]);
    slope_att.read(PredType::NATIVE_FLOAT, slope.data());
    itcpt_att.read(PredType::NATIVE_FLOAT, intercept.data());
    for (auto& value : res)
        value = value * slope[acpc] + intercept[acpc];
    return {res, dims.first};
}

/// @brief 读取 2D 数据集 (INT16 -> float 定标)
inline pair<vector<float>, vector<hsize_t>> rer2D_base(const DataSet& src) {
    auto dims = read_dims(src);
    const size_t count = static_cast<size_t>(dims.second.getSimpleExtentNpoints());
    vector<float> res(count);
    src.read(res.data(), PredType::NATIVE_FLOAT);
    Attribute slope_att = src.openAttribute("Slope"), itcpt_att = src.openAttribute("Intercept");
    float slope, intercept;
    slope_att.read(PredType::NATIVE_FLOAT, &slope);
    itcpt_att.read(PredType::NATIVE_FLOAT, &intercept);
    for (auto& value : res)
        value = value * slope + intercept;
    return {res, dims.first};
}

/// @brief 读取 2D 数据集 (UINT16)
inline pair<vector<uint16_t>, vector<hsize_t>> rer2D_raw(const DataSet& src) {
    auto dims = read_dims(src);
    vector<uint16_t> rawdata(dims.second.getSimpleExtentNpoints());
    src.read(rawdata.data(), PredType::NATIVE_UINT16);
    return {rawdata, dims.first};
}

/// @brief 读取 2D 数据集 (F32 -> float 定标)
inline pair<vector<float>, vector<hsize_t>> rer2D_f32_base(const DataSet& src) {
    auto dims = read_dims(src);
    const size_t count = static_cast<size_t>(dims.second.getSimpleExtentNpoints());
    vector<float> res(count);
    src.read(res.data(), PredType::NATIVE_FLOAT);
    Attribute slope_att = src.openAttribute("Slope"), itcpt_att = src.openAttribute("Intercept");
    float slope, intercept;
    slope_att.read(PredType::NATIVE_FLOAT, &slope);
    itcpt_att.read(PredType::NATIVE_FLOAT, &intercept);
    for (auto& value : res)
        value = value * slope + intercept;
    return {res, dims.first};
}

template<class Tp, class To>
pair<vector<To>, vector<hsize_t>> rer_all_base(const DataSet& src, const DataType& Tptyp,
                                               const DataType& Totyp) {
    auto dims = read_dims(src);
    const size_t count = static_cast<size_t>(dims.second.getSimpleExtentNpoints());
    vector<To> res(count);
    src.read(res.data(), Totyp);
    Attribute slope_att = src.openAttribute("Slope"),
            itcpt_att = src.openAttribute("Intercept");
    To slope, intercept;
    slope_att.read(Totyp, &slope);
    itcpt_att.read(Totyp, &intercept);
    for (auto& value : res)
        value = value * slope + intercept;
    return {res, dims.first};
}

/// @brief 读取 2D 系数表中的单行
inline vector<float> read_2D_single_linef(const DataSet& coeff, hsize_t linum) {
    auto dims = read_dims(coeff);
    assert(linum < dims.first[0]);
    hsize_t start[3]{linum, 0}, count[3]{1, dims.first[1]};
    DataSpace rd = coeff.getSpace(), ms(2, count);
    rd.selectHyperslab(H5S_SELECT_SET, count, start);
    vector<float> rawdata(count[1]);
    coeff.read(rawdata.data(), PredType::NATIVE_FLOAT, ms, rd);
    return rawdata;
}

/// @brief  读取反射率通道并完成基础定标
inline pair<vector<float>, vector<hsize_t>> read_refl(const DataSet& src, const DataSet& VIS_Cal_Coeff, hsize_t bdid,
                                                      hsize_t vcid) {
    auto DN = rer_base(src, bdid);
    auto coeff = read_2D_single_linef(VIS_Cal_Coeff, vcid);
    for (auto& i : DN.first)
        i = coeff[2] * i * i + coeff[1] * i + coeff[0];
    return DN;
}

/// @brief  读取反射率通道并完成基础定标
inline pair<vector<float>, vector<hsize_t>> read_refl_qk(const DataSet& src, const DataSet& VIS_Cal_Coeff,
                                                         hsize_t vcid) {
    auto DN = rer2D_base(src);
    auto coeff = read_2D_single_linef(VIS_Cal_Coeff, vcid);
    for (auto& i : DN.first)
        i = coeff[2] * i * i + coeff[1] * i + coeff[0];
    return DN;
}

/// @brief  读取经纬度地理定位数据
inline pair<vector<float>, vector<hsize_t>> read_geolocation(const DataSet& src) {
    auto dims = read_dims(src);
    vector<float> res(dims.second.getSimpleExtentNpoints());
    src.read(res.data(), PredType::NATIVE_FLOAT);
    return {res, dims.first};
}

inline string read_str_attr(const Attribute& h5attr) {
    string res;
    h5attr.read(h5attr.getDataType(), res);
    return res;
}

inline uint16_t read_u16_attr(const Attribute& h5attr) {
    uint16_t res;
    h5attr.read(PredType::NATIVE_UINT16, &res);
    return res;
}

/// @brief 读取有符号 32 位整数属性
inline int32_t read_i32_attr(const Attribute& h5attr) {
    int32_t res;
    h5attr.read(PredType::NATIVE_INT32, &res);
    return res;
}

/// @brief 读取 FY-3D MWRI 指定通道的亮温，并完成 DN 定标
///
/// FY-3D 文档将亮温数据描述为 [scan, pixel, channel]，但实际发布的
/// HDF5 样例使用 [channel, scan, pixel]。这里按长度为 10 的轴自动识别
/// 通道维度，因此兼容文档描述和实际样例两种排列。
inline pair<vector<float>, vector<hsize_t>> read_mwri_fy3d_channel(const DataSet& src, hsize_t channel) {
    auto dims = read_dims(src).first;
    if (dims.size() != 3)
        throw invalid_argument("FY-3D MWRI 亮温数据集必须是三维数组");

    int channel_axis = -1;
    for (int axis = 0; axis < 3; ++axis)
        if (dims[axis] == 10) {
            channel_axis = axis;
            break;
        }
    if (channel_axis < 0)
        throw invalid_argument("FY-3D MWRI 亮温数据集未找到长度为 10 的通道维度");
    if (channel >= dims[channel_axis])
        throw out_of_range("FY-3D MWRI 通道编号必须在 0~9 之间");

    vector<hsize_t> start(3, 0), count = dims;
    start[channel_axis] = channel;
    count[channel_axis] = 1;
    DataSpace rd = src.getSpace(), ms(3, count.data());
    rd.selectHyperslab(H5S_SELECT_SET, count.data(), start.data());

    size_t pixels = 1;
    for (int axis = 0; axis < 3; ++axis)
        if (axis != channel_axis)
            pixels *= static_cast<size_t>(dims[axis]);
    vector<int16_t> raw(pixels);
    src.read(raw.data(), PredType::NATIVE_INT16, ms, rd);

    float slope = 1.0f, intercept = 0.0f;
    src.openAttribute("Slope").read(PredType::NATIVE_FLOAT, &slope);
    src.openAttribute("Intercept").read(PredType::NATIVE_FLOAT, &intercept);

    int32_t fill_value = -32767;
    if (src.attrExists("FillValue"))
        fill_value = read_i32_attr(src.openAttribute("FillValue"));
    int32_t valid_min = INT32_MIN, valid_max = INT32_MAX;
    if (src.attrExists("valid_range")) {
        Attribute valid_range = src.openAttribute("valid_range");
        int32_t range[2];
        valid_range.read(PredType::NATIVE_INT32, range);
        valid_min = range[0];
        valid_max = range[1];
    }

    vector<float> result(pixels);
    for (size_t i = 0; i < pixels; ++i)
        if (raw[i] == fill_value || raw[i] < valid_min || raw[i] > valid_max)
            result[i] = NAN;
        else
            result[i] = static_cast<float>(raw[i]) * slope + intercept;

    vector<hsize_t> result_dims;
    result_dims.reserve(2);
    for (int axis = 0; axis < 3; ++axis)
        if (axis != channel_axis)
            result_dims.push_back(dims[axis]);
    return {std::move(result), std::move(result_dims)};
}

/// @brief  构造渲染标签
/// @return [File Name, Date, Time(UTC), Sat+Sensor, long_name]
inline vector<string> read_labels(const H5File& file, const DataSet& ds, bool has_long_name) {
    Group g = file.openGroup("/");
    string time = read_str_attr(g.openAttribute("Observing Beginning Time"));
    while (!time.empty() && time.back() != '.') time.pop_back();
    if (!time.empty()) time.pop_back();
    time += "(UTC)";
    string last = read_str_attr(ds.openAttribute("long_name"));
    if (!has_long_name)
        last.clear();
    return {
        read_str_attr(g.openAttribute("File Name")),
        read_str_attr(g.openAttribute("Observing Beginning Date")),
        time,
        read_str_attr(g.openAttribute("Satellite Name")) + " " +
        read_str_attr(g.openAttribute("Sensor Identification Code")),
        last
    };
}

/// @brief 读取文件根目录下的 TBB_Trans_Coefficient 属性数组
/// @param file HDF5 数据文件对象
/// @return 亮温转换系数数组
inline vector<float> read_tbb_trans_coeff(const H5File& file, const string& attrname) {
    Attribute attr = file.openAttribute(attrname);
    int n = attr.getSpace().getSimpleExtentNpoints();
    vector<float> res(n);
    attr.read(PredType::NATIVE_FLOAT, res.data());
    return res;
}

/// @brief 读取指定波段的亮温数据   (单位 K)，自动提取 TBB 系数
/// @param file HDF5 数据文件对象
/// @param band_cfg 卫星波段配置信息 (需包含 transid 和 mersi_equiv_mid_wn)
/// @return pair<亮温数据数组,维度信息>
inline pair<pair<vector<float>, vector<hsize_t>>, DataSet> read_emissive(
    const H5File& file,
    const SatConfig::BandConfig& band_cfg, const string& sat, bool isqk = false) {
    float A, B;
    if (sat == "FY-3D")
    {
        auto tbb_a = read_tbb_trans_coeff(file, "TBB_Trans_Coefficient_A");
        auto tbb_b = read_tbb_trans_coeff(file, "TBB_Trans_Coefficient_B");
        A = tbb_a.at(band_cfg.transid);
        B = tbb_b.at(band_cfg.transid);
    }
    else
    {
        auto tbb_coeff = read_tbb_trans_coeff(file, "TBB_Trans_Coefficient");
        A = tbb_coeff.at(band_cfg.transid);
        B = tbb_coeff.at(band_cfg.transid + tbb_coeff.size() / 2);
    }

    string dataset = "Data/" + (isqk ? band_cfg.qkdataset : band_cfg.akdataset);
    DataSet src = file.openDataSet(dataset);
    pair<vector<float>, vector<hsize_t>> res;
    if (isqk)
    {
        res = rer2D_base(src);
        res.second.insert(res.second.begin(), 0);
    }
    else
        res = rer_base(src, band_cfg.akid);

    for (auto& i : res.first)
    {
        double te = brtemp(i, band_cfg.mersi_equiv_mid_wn);
        i = te * A + B;
    }
    return pair{res, src};
}

/// @brief  读取指定波段的反射率数据，利用 SatConfig 自动映射定标系数索引
/// @param  file HDF5 数据文件对象
/// @param  band_cfg 卫星波段配置信息
/// @return pair<反射率数据数组,维度信息>
inline pair<pair<vector<float>, vector<hsize_t>>, DataSet> read_reflectance_configured(
    const H5File& file,
    const SatConfig::BandConfig& band_cfg, string coeffDSname, bool isqk = false) {
    DataSet src;
    if (isqk)
        src = file.openDataSet("Data/" + band_cfg.qkdataset);
    else
        src = file.openDataSet("Data/" + band_cfg.akdataset);
    DataSet coeffDs = file.openDataSet("Calibration/" + coeffDSname);
    if (isqk)
    {
        auto res = read_refl_qk(src, coeffDs, band_cfg.transid);
        res.second.insert(res.second.begin(), 0);
        return {res, src};
    }
    return {read_refl(src, coeffDs, band_cfg.akid, band_cfg.transid), src};
}

inline pair<pair<vector<float>, vector<hsize_t>>, DataSet> read_fy4(
    const H5File& file, int band, float fill_v = NAN) {
    string post_cnlid;
    if (band < 10)
        post_cnlid = "Channel0" + to_string(band);
    else
        post_cnlid = "Channel" + to_string(band);
    DataSet src = file.openDataSet("Data/NOM" + post_cnlid);
    auto raw = rer2D_raw(src);
    DataSet coeffDs = file.openDataSet("Calibration/CAL" + post_cnlid);
    vector<float> table(coeffDs.getSpace().getSimpleExtentNpoints());
    coeffDs.read(table.data(), PredType::NATIVE_FLOAT);
    vector<float> res(raw.first.size());
    for (size_t i = 0; i < raw.first.size(); ++i)
        if (raw.first[i] >= table.size())
            res[i] = fill_v;
        else
            res[i] = table[raw.first[i]];
    return {{std::move(res), raw.second}, src};
}

pair<vector<float>, vector<hsize_t>> interp(const pair<vector<float>, vector<hsize_t>>& inp,
                                            hsize_t target_rows, hsize_t target_cols) {
    const int source_rows = static_cast<int>(inp.second[0]);
    const int source_cols = static_cast<int>(inp.second[1]);
    const int rows = static_cast<int>(target_rows);
    const int cols = static_cast<int>(target_cols);
    vector<float> result(static_cast<size_t>(target_rows * target_cols));
    cv::Mat src(source_rows, source_cols, CV_32F, const_cast<float*>(inp.first.data()));
    cv::Mat dst(rows, cols, CV_32F, result.data());
    cv::resize(src, dst, cv::Size(cols, rows), 0, 0, cv::INTER_CUBIC);
    return {std::move(result), {target_rows, target_cols}};
}

vector<float> interp(const vector<float>& inp, const vector<hsize_t>& inp_dims, hsize_t target_rows,
                     hsize_t target_cols) {
    const int source_rows = static_cast<int>(inp_dims[0]);
    const int source_cols = static_cast<int>(inp_dims[1]);
    const int rows = static_cast<int>(target_rows);
    const int cols = static_cast<int>(target_cols);
    vector<float> result(static_cast<size_t>(target_rows * target_cols));
    cv::Mat src(source_rows, source_cols, CV_32F, const_cast<float*>(inp.data()));
    cv::Mat dst(rows, cols, CV_32F, result.data());
    cv::resize(src, dst, cv::Size(cols, rows), 0, 0, cv::INTER_CUBIC);
    return result;
}

/// @brief 从 1KM 地理文件中读取float->FLOAT并插值到目标分辨率
inline std::vector<float> read_and_interp_geo_float(const H5File& geofile, const DataSet& ds,
                                                    int target_rows, int target_cols, bool islon = false) {
    auto [data, dims] = rer2D_f32_base(ds);
    if (islon && *ranges::max_element(data) -
        *ranges::min_element(data) > 180)
        for (auto& i : data)
            if (i < 0)
                i += 360;
    int src_rows = static_cast<int>(dims[0]);
    int src_cols = static_cast<int>(dims[1]);
    std::vector<float> result(static_cast<size_t>(target_rows) * target_cols);
    cv::Mat src(src_rows, src_cols, CV_32F, data.data());
    cv::Mat dst(target_rows, target_cols, CV_32F, result.data());
    cv::resize(src, dst, cv::Size(target_cols, target_rows), 0, 0, cv::INTER_CUBIC);
    return result;
}

/// @brief  从 1KM 地理文件中读取int16->FLOAT并插值到目标分辨率
inline std::vector<float> read_and_interp_geo_i16(const H5File& geofile, const DataSet& ds,
                                                  int target_rows, int target_cols) {
    auto [data, dims] = rer2D_base(ds);
    int src_rows = static_cast<int>(dims[0]);
    int src_cols = static_cast<int>(dims[1]);
    std::vector<float> result(static_cast<size_t>(target_rows) * target_cols);
    cv::Mat src(src_rows, src_cols, CV_32F, data.data());
    cv::Mat dst(target_rows, target_cols, CV_32F, result.data());
    cv::resize(src, dst, cv::Size(target_cols, target_rows), 0, 0, cv::INTER_CUBIC);
    return result;
}

/// @brief  读取太阳天顶角，250M 模式下插值到目标分辨率
inline pair<vector<float>, vector<hsize_t>> read_solar_zenith(const H5File& geofile, bool isQkm, int scln, int pps) {
    DataSet ds = geofile.openDataSet("Geolocation/SolarZenith");
    if (isQkm)
    {
        auto data = read_and_interp_geo_i16(geofile, ds, scln, pps);
        return {data, {(hsize_t) scln, (hsize_t) pps}};
    }
    return rer2D_base(ds);
}

inline pair<vector<float>, vector<hsize_t>> read_solar_zenith_fy4(const H5File& geofile, int scln, int pps) {
    DataSet ds = geofile.openDataSet("Navigation/NOMSunZenith");
    auto data = read_and_interp_geo_float(geofile, ds, scln, pps);
    return {data, {(hsize_t) scln, (hsize_t) pps}};
}

/// @brief 读取太阳方位角
inline pair<vector<float>, vector<hsize_t>> read_solar_azimuth(const H5File& geofile) {
    DataSet ds = geofile.openDataSet("Geolocation/SolarAzimuth");
    return rer2D_base(ds);
}

inline pair<vector<float>, vector<hsize_t>> read_solar_azimuth_fy4(const H5File& geofile, int scln, int pps) {
    DataSet ds = geofile.openDataSet("Navigation/NOMSunAzimuth");
    auto data = read_and_interp_geo_float(geofile, ds, scln, pps);
    return {data, {(hsize_t) scln, (hsize_t) pps}};
}

/// @brief 读取卫星天顶角 (FY-4 GEO 文件)
inline pair<vector<float>, vector<hsize_t>> read_satellite_zenith_fy4(const H5File& geofile, int scln, int pps) {
    DataSet ds = geofile.openDataSet("Navigation/NOMSatelliteZenith");
    auto data = read_and_interp_geo_float(geofile, ds, scln, pps);
    return {data, {(hsize_t) scln, (hsize_t) pps}};
}

/// @brief 读取日地距离比
inline double read_earth_sun_distance(const H5File& file, string dsname = "EarthSun Distance Ratio") {
    double Des = 1.0;
    Attribute attr = file.openAttribute(dsname);
    attr.read(PredType::NATIVE_DOUBLE, &Des);
    return Des;
}

/// @brief FY-4 标称投影（NOM）导航参数，全部来自文件私有属性
struct NomNavParams {
    double center_lat = 0;      ///< NOMCenterLat
    double center_lon = 0;      ///< NOMCenterLon（度）
    double sat_height = 0;      ///< NOMSatHeight（米，星下点海拔）
    double dEA = 0;             ///< 地球赤道半径（米）
    double inv_flat = 0;        ///< 地球扁率倒数
    double dSamplingAngle = 0;  ///< 东西采样角（微弧度）
    double dSteppingAngle = 0;  ///< 南北步进角（微弧度）
};

/// @brief 读取 FY-4 导航私有属性（不依赖文件名）
inline NomNavParams read_fy4_nav_params(const H5File& file) {
    NomNavParams p;
    float tmp = 0;
    Attribute att = file.openAttribute("NOMCenterLat");
    att.read(PredType::NATIVE_FLOAT, &tmp);
    p.center_lat = tmp;
    att = file.openAttribute("NOMCenterLon");
    att.read(PredType::NATIVE_FLOAT, &tmp);
    p.center_lon = tmp;
    att = file.openAttribute("NOMSatHeight");
    att.read(PredType::NATIVE_FLOAT, &tmp);
    p.sat_height = tmp;
    att = file.openAttribute("dEA");
    att.read(PredType::NATIVE_DOUBLE, &p.dEA);
    att = file.openAttribute("dObRecFlat");
    att.read(PredType::NATIVE_DOUBLE, &p.inv_flat);
    att = file.openAttribute("dSamplingAngle");
    att.read(PredType::NATIVE_DOUBLE, &p.dSamplingAngle);
    att = file.openAttribute("dSteppingAngle");
    att.read(PredType::NATIVE_DOUBLE, &p.dSteppingAngle);
    return p;
}

/// @brief NOM 扫描角反算大地经纬度（视线与椭球求交，标准静止卫星投影）
/// @param x 东西扫描角（弧度，向东为正）
/// @param y 南北扫描角（弧度，向北为正）
inline void nom_scan_to_latlon(double x, double y, const NomNavParams& p, float& lat, float& lon) {
    const double a = p.dEA;
    const double f = 1.0 / p.inv_flat;
    const double e2 = f * (2.0 - f);
    const double b2 = a * a * (1.0 - e2);
    const double H = p.sat_height + a;
    const double cosx = cos(x), sinx = sin(x), cosy = cos(y), siny = sin(y);
    const double A2 = cosy * cosy + (a * a / b2) * siny * siny;
    const double disc = H * H * cosx * cosx * cosy * cosy - A2 * (H * H - a * a);
    if (disc < 0)   // 视线不穿过地球（盘外）
    {
        lat = lon = NAN;
        return;
    }
    const double sn = (H * cosx * cosy - sqrt(disc)) / A2;   // 近交点距离
    const double xp = sn * siny;                             // 北向分量
    const double yp = sn * sinx * cosy;                      // 东向分量
    const double zp = H - sn * cosx * cosy;                  // 指向卫星方向分量
    lat = static_cast<float>(atan2(xp, (1.0 - e2) * hypot(yp, zp)) / Deg2rad);
    lon = static_cast<float>(p.center_lon + atan2(yp, zp) / Deg2rad);
}

/// @brief 按行/列窗口计算 FY-4 全圆盘经纬度（行 0 为最北、列 0 为最西）
inline pair<vector<float>, vector<float>> read_fy4_latlon(const H5File& geofile, int rows, int cols) {
    auto p = read_fy4_nav_params(geofile);
    vector<float> lat(static_cast<size_t>(rows) * cols), lon(static_cast<size_t>(rows) * cols);
    const double c0 = (cols - 1) / 2.0, l0 = (rows - 1) / 2.0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
        {
            const double x = (c - c0) * p.dSamplingAngle * 1e-6;
            const double y = (l0 - r) * p.dSteppingAngle * 1e-6;
            nom_scan_to_latlon(x, y, p, lat[static_cast<size_t>(r) * cols + c],
                               lon[static_cast<size_t>(r) * cols + c]);
        }
    return {lat, lon};
}

#endif
