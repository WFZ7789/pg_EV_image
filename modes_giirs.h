#ifndef PG_EV_IMAGE_MODES_GIIRS_H
#define PG_EV_IMAGE_MODES_GIIRS_H

#include "stl.h"
#include "phys.h"
#include "color.h"
#include "read.h"
#include "rend.h"
#include "context.h"

using namespace std;
using namespace H5;

inline void mode_giirs(Context& ctx, const string& mode) {
    if (mode == "R")
    {
        DataSet vis_dn = ctx.file.openDataSet("Data/VIS_DN");
        auto [dn, dims] = rer2D_base(vis_dn);
        auto caltable = rer_all_base<float, float>(ctx.file.openDataSet("Data/VIS_CalTable"),
                                                   PredType::NATIVE_FLOAT, PredType::NATIVE_FLOAT).first;
        for (int i = 0; i < dn.size(); i++)
            dn[i] = dn[i] * dn[i] * caltable[i * 3] + dn[i] *
                    caltable[i * 3 + 1] + caltable[i * 3 + 2];
        DataSet latitude = ctx.geofile.openDataSet("Geolocation/Latitude_VIS");
        DataSet longitude = ctx.geofile.openDataSet("Geolocation/Longitude_VIS");
        auto lat = read_geolocation(latitude).first;
        auto lon = read_geolocation(longitude).first;
        dims.insert(dims.begin(), 0);
        rend(dims, dn, lat, lon, ctx.out_name, read_labels(ctx.file, vis_dn, true),
             "", color_refsb, {0, percentile(dn, 99)}, ctx.crop, ctx.target_lat,
             ctx.target_lon, ctx.target_square_size);
    }
    else if (mode == "LW" || mode == "MW")
    {
        DataSet realir = ctx.file.openDataSet("Data/ES_Real" + mode);
        auto data = read_2D_single_linef(realir, ctx.band_id);
        DataSet wn = ctx.file.openDataSet("Data/WN_" + mode);
        float wavenumber = rer_all_base<float, float>(wn,
                                                      PredType::NATIVE_FLOAT, PredType::NATIVE_FLOAT).first.at(
            ctx.band_id);
        for (auto& i : data)
            i = brtemp(i, wavenumber);
        DataSet latitude = ctx.geofile.openDataSet("Geolocation/Latitude_" + mode);
        DataSet longitude = ctx.geofile.openDataSet("Geolocation/Longitude_" + mode);
        auto lat = read_geolocation(latitude).first;
        auto lon = read_geolocation(longitude).first;
        vector<hsize_t> dims(3, 0);
        dims[1] = ctx.pps;
        dims[2] = ctx.scln;
        auto mmx = ranges::minmax_element(data);
        auto lbls = read_labels(ctx.file, realir, true);
        lbls.back() += " " + to_string(wavenumber) + "cm^-1";
        RenderConfig cfg{*mmx.min, *mmx.max};
        rend(dims, data, lat, lon, ctx.out_name, lbls,
             "", color_plasma, cfg, ctx.crop, ctx.target_lat, ctx.target_lon,
             ctx.target_square_size);
    }
    else if (mode == "IRall")
    {
        auto run = [&] (const string& mod, int init_picid) {
            DataSet realir = ctx.file.openDataSet("Data/ES_Real" + mod);
            auto [data, dims] = rer2D_f32_base(realir);
            DataSet wn = ctx.file.openDataSet("Data/WN_" + mod);
            auto wavenumber = rer_all_base<float, float>(wn,
                                                         PredType::NATIVE_FLOAT, PredType::NATIVE_FLOAT).first;
            for (int i = 0; i < dims.at(0); i++)
                for (int j = 0; j < dims[1]; j++)
                    data[i * dims[1] + j] = brtemp(data[i * dims[1] + j], wavenumber.at(i));
            DataSet latitude = ctx.geofile.openDataSet("Geolocation/Latitude_" + mod);
            DataSet longitude = ctx.geofile.openDataSet("Geolocation/Longitude_" + mod);
            auto lat = read_geolocation(latitude).first;
            auto lon = read_geolocation(longitude).first;
            auto mmx = ranges::minmax_element(data);
            vector<hsize_t> matdims(3);
            matdims[1] = ctx.pps;
            matdims[2] = ctx.scln;
            for (int i = 0; i < dims[0]; i++)
            {
                stringstream outname;
                outname << setw(6) << setfill('0') << i + init_picid;
                cout << '[' << outname.str() << "]\n";
                auto lbls = read_labels(ctx.file, realir, true);
                lbls.back() += " " + to_string(wavenumber.at(i)) + "cm^-1";
                RenderConfig cfg{*mmx.min, *mmx.max};

                rend(matdims, vector(data.begin() + i * dims[1], data.end() + (i + 1) * dims[1]),
                     lat, lon, outname.str(), lbls,
                     "", color_plasma, cfg, ctx.crop, ctx.target_lat, ctx.target_lon,
                     ctx.target_square_size);
            }
            return dims[0];
        };
        int mwend = run("MW", 0);
        run("LW", mwend);
    }
}

#endif
