/// SPDX-License-Identifier: MIT

#include    "stl.h"
#include    "phys.h"
#include    "color.h"
#include    "read.h"
#include    "rend.h"
#include    "config.h"
#include    "modes.h"

using namespace std;
using namespace H5;

int legacy_main(int argc, char** argv) {
    CLI::App app{"FY 系列图像处理"};

    string mode;
    string data_file;
    string geo_file;
    string out_name;
    string aux_file;
    string color_scheme;
    bool crop = false;
    double target_range = 7.5;
    int band_id = 0;
    double target_lat = 0, target_lon = 0;
    bool aq_prg = false;

    app.add_option("-m,--mode", mode, "E(热辐射), R(反射), RT(真彩色),"
                   "RF(假彩色), EC(热辐射合成), MWRI: S/W/W84/TCX/TCN/TCN2/TCS/TCR, PMR")->required();
    app.add_option("-d,--data", data_file, "数据文件路径")->required();
    app.add_option("-g,--geo", geo_file, "地理文件路径");
    app.add_option("-a,--aux", aux_file, "附加文件路径");
    app.add_option("-o,--out", out_name, "不包含扩展名的输出路径");
    app.add_option("-b,--band-id", band_id, "Band 编号（FY-3D MWRI 为 0~9）");
    app.add_option("-c,--color", color_scheme, "色阶: CA/PLASMA/VIS/REFSB，缺省按模式默认");
    app.add_flag("--crop", crop, "按 -x/-y 中心裁剪目标区域（MERSI、FY-4B AGRI/GIIRS 全模式）");
    app.add_option("-r,--range", target_range, "目标区域半宽（度），默认 7.5");
    app.add_flag("--aq", aq_prg, "使用蒸饺投影");
    app.add_option("-x,--target-lat", target_lat, "中心纬度（-90~90）");
    app.add_option("-y,--target-lon", target_lon, "中心经度（0~360）");
    CLI11_PARSE(app, argc, argv);

    if (geo_file.empty())
        geo_file = data_file;
    H5File file(data_file, H5F_ACC_RDONLY);
    H5File geofile(geo_file, H5F_ACC_RDONLY);

    string sat = read_str_attr(file.openGroup("/").openAttribute("Satellite Name"));

    Context ctx{
        file, geofile, aux_file, sat, "",
        out_name, band_id, target_lat, target_lon
    };
    ctx.color_scheme = color_scheme;
    ctx.crop = crop;
    ctx.aq = aq_prg;
    ctx.target_square_size = static_cast<float>(target_range);

    if (mode == "PMR")
    {
        mode_PMR(ctx);
        return 0;
    }
    ctx.sen_code = read_str_attr(file.openAttribute("Sensor Identification Code"));
    if (ctx.sen_code.contains("MWRI"))
    {
        try {
            mode_MWRI(ctx, mode);
        }
        catch (const exception& ex) {
            cerr << "MWRI 处理失败: " << ex.what() << endl;
            return 2;
        }
        return 0;
    }
    else if (sat.substr(0, 4) == "FY-4")
    {
        ctx.scln = read_u16_attr(file.openAttribute("End Line Number")) - read_u16_attr(
                       file.openAttribute("Begin Line Number")) + 1;
        ctx.pps = read_u16_attr(file.openAttribute("End Pixel Number")) - read_u16_attr(
                      file.openAttribute("Begin Pixel Number")) + 1;
        ctx.isQkm = read_str_attr(file.openAttribute("File Alias Name")).find("2000M") !=
                    string::npos;
        if (ctx.sen_code == "GIIRS")
            mode_giirs(ctx, mode);
        else
            dispatch_fy4(ctx, mode);
        return 0;
    }

    string alias_filename = read_str_attr(file.openGroup("/").openAttribute("File Alias Name"));
    bool isQkm = alias_filename.find("250M") != string::npos ||
                 alias_filename.find("QKM") != string::npos;
    ctx.isQkm = isQkm;

    DataSet latitude = geofile.openDataSet("Geolocation/Latitude");
    DataSet longitude = geofile.openDataSet("Geolocation/Longitude");
    int scln = read_u16_attr(file.openAttribute("Scan_Line_number")),
            pps = read_u16_attr(file.openAttribute("Pixels_per_Scan"));
    if (isQkm && sat == "FY-3D")
        pps = 8192;
    vector<float> lat, lon;
    if (!isQkm)
    {
        lat = read_geolocation(latitude).first;
        lon = read_geolocation(longitude).first;
        if (*ranges::max_element(lon) - *ranges::min_element(lon) > 180)
            for (auto& i : lon)
                if (i < 0)
                    i += 360;
    }
    else
    {
        lat = read_and_interp_geo_float(geofile, latitude, scln, pps);
        lon = read_and_interp_geo_float(geofile, longitude, scln, pps, true);
    }
    if (aq_prg)
    {
        target_lat *= Deg2rad;
        target_lon *= Deg2rad;
        for (int i = 0; i < lat.size(); i++)
        {
            double x = lat[i] * Deg2rad;
            double y = lon[i] * Deg2rad;

            double dist = acos(sin(target_lat) * sin(x) +
                               cos(target_lat) * cos(x) * cos(y - target_lon));
            double angle = atan2(sin(y - target_lon) * cos(x),
                                 cos(target_lat) * sin(x) - sin(target_lat) * cos(x) * cos(y - target_lon));

        double R = sin(dist);
        double screen_x = R * sin(angle);
        double screen_y = R * cos(angle);
        // 正交投影输出从弧度换算为度，投影中心保持 (0,0)
        lon[i] = static_cast<float>(screen_x / Deg2rad);
        lat[i] = static_cast<float>(screen_y / Deg2rad);
        }
    }

    ctx.lat = lat;
    ctx.lon = lon;
    ctx.scln = scln;
    ctx.pps = pps;

    dispatch(ctx, mode);
    return 0;
}
