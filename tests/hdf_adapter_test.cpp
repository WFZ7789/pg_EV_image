/// SPDX-License-Identifier: MIT

#include "core/calibration.hpp"
#include "sensors/hdf.hpp"
#include "hdf_fixture.hpp"

#include <algorithm>
#include <cmath>
#include <source_location>

namespace {

using namespace fy::core;

void require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("HDF adapter check failed at line " + std::to_string(location.line()));
}

template<class F>
void rejects(F&& action) {
    try { action(); }
    catch (...) { return; }
    throw std::runtime_error("Expected malformed HDF input to be rejected");
}

void mersi_file(const std::filesystem::path& path, bool quarter, bool malformed = false) {
    using namespace hdf_fixture;
    H5::H5File file(path.string(), H5F_ACC_TRUNC);
    metadata(file, "FY-3H", "MERSI", quarter ? "0250M" : "1000M");
    if (quarter) numbers(file, "TBB_Trans_Coefficient", {1,1,1,1,1.7,1, 0,0,0,0,8,0});
    else numbers(file, "TBB_Trans_Coefficient", {1,1,1,1,0.8,1, 0,0,0,0,-2,0});
    geolocation(file, "Geolocation", 3, 5, true);
    const std::size_t rows = quarter ? 12 : 3, cols = quarter ? 20 : 5;
    std::vector<float> raw(rows * cols);
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = 20 + i % 40;
    raw[1] = 65535;
    const std::vector<hsize_t> dimensions = quarter ? std::vector<hsize_t>{rows, cols} :
                                                                   std::vector<hsize_t>{1, rows, cols};
    auto thermal = dataset(file, quarter ? "Data/EV_250_Emissive_b24" : "Data/EV_250_Aggr.1KM_Emissive",
                            dimensions, raw);
    numbers(thermal, "Slope", {2}); numbers(thermal, "Intercept", {1});
    numbers(thermal, "FillValue", {65535}); numbers(thermal, "valid_range", {0, 65500});
    if (!quarter) {
        const hsize_t reflectance_dimensions[]{4, rows, cols};
        std::vector<float> reflectance(4 * rows * cols);
        for (std::size_t i = 0; i < reflectance.size(); ++i) reflectance[i] = static_cast<float>(i);
        auto refl = dataset(file, "Data/EV_250_Aggr.1KM_RefSB", reflectance_dimensions, reflectance);
        if (malformed) numbers(refl, "Slope", {2, 3});
        else numbers(refl, "Slope", {2,3,4,5});
        numbers(refl, "Intercept", {-1,1,2,3});
        const hsize_t coefficient_dimensions[]{19, 3};
        std::vector<float> coefficients(57);
        for (std::size_t band = 0; band < 19; ++band) {
            coefficients[band * 3] = 0.5f;
            coefficients[band * 3 + 1] = 2;
            coefficients[band * 3 + 2] = 0.125f;
        }
        dataset(file, "Calibration/VIS_Cal_Coeff", coefficient_dimensions, coefficients);
        const hsize_t angle_dimensions[]{rows, cols};
        std::vector<float> angle(rows * cols, 6000);
        auto solar = dataset(file, "Geolocation/SolarZenith", angle_dimensions, angle);
        numbers(solar, "Slope", {0.01}); numbers(solar, "Intercept", {0});
    }
}

void mwri_file(const std::filesystem::path& path, int axis, bool second_generation) {
    using namespace hdf_fixture;
    H5::H5File file(path.string(), H5F_ACC_TRUNC);
    metadata(file, second_generation ? "FY-3H" : "FY-3D", second_generation ? "MWRI-II" : "MWRI");
    for (std::size_t channels : second_generation ? std::vector<std::size_t>{10, 12} : std::vector<std::size_t>{10}) {
        const std::string prefix = second_generation ? (channels == 10 ? "Window Channel/" : "Sounding Channel/") : "";
        geolocation(file, prefix + "Geolocation", 3, 5);
        std::vector<hsize_t> dimensions{3, 5};
        dimensions.insert(dimensions.begin() + axis, channels);
        std::vector<float> values(channels * 3 * 5);
        for (std::size_t channel = 0; channel < channels; ++channel)
            for (std::size_t row = 0; row < 3; ++row)
                for (std::size_t col = 0; col < 5; ++col) {
                    std::array<std::size_t, 3> index;
                    std::size_t spatial_axis = 0;
                    for (int dimension = 0; dimension < 3; ++dimension)
                        index[dimension] = dimension == axis ? channel : spatial_axis++ == 0 ? row : col;
                    values[(index[0] * dimensions[1] + index[1]) * dimensions[2] + index[2]] =
                        row == 0 && col == 0 ? -32767.0f : row == 0 && col == 1 ? 25000.0f :
                        static_cast<float>(1000 + channel * 100 + row * 10 + col);
                }
        const auto name = second_generation ? prefix + "Calibration/EARTH_OBSERVE_BT" :
                                               "Calibration/EARTH_OBSERVE_BT_10_to_89GHz";
        auto data = dataset(file, name, dimensions, values, H5::PredType::NATIVE_INT16);
        numbers(data, "Slope", {0.01}); numbers(data, "Intercept", {100});
        numbers(data, "FillValue", {-32767}); numbers(data, "valid_range", {0, 20000});
    }
}

}

int main() {
    ExecutionContext context({});
    const auto predefined_types = context.io().run([] {
        static_cast<void>(H5::PredType::NATIVE_FLOAT.getSize());
        return H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_DATATYPE);
    });
    AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry, context.profile());
    const auto directory = context.scratch().path();
    const auto main = directory / "mersi_1000M.HDF", quarter = directory / "mersi_0250M.HDF";
    context.io().run([&] { mersi_file(main, false); mersi_file(quarter, true); });
    {
        auto session = registry.open("mersi", {main, main, quarter});
        const auto bands = session.bands();
        auto band = [&](int id) -> const BandDescriptor& {
            const auto found = std::ranges::find(bands, id, &BandDescriptor::id);
            if (found == bands.end()) throw std::runtime_error("Missing synthetic band");
            return *found;
        };
        require(session.metadata().grid("native").shape == Shape{3, 5});
        require(session.metadata().resolution == "1000M");
        require(session.metadata().grid("auxiliary").shape == Shape{12, 20});
        const auto before = context.profile().snapshot().io.value();
        auto samples = session.read_window(24, {1, 1, {2, 3}});
        const auto after = context.profile().snapshot().io.value();
        require(after.read_calls - before.read_calls == 1 && after.read_samples - before.read_samples == 6);
        require(samples.values.view().at(0, 0) == 26);
        calibrate(samples, band(24).calibration, context);
        const float radiance = 26.0f * 2.0f + 1.0f;
        const float expected = static_cast<float>(1.4387768775039337 * 931.601394 /
            std::log(1.1910429723971884140794892e-5 * std::pow(931.601394, 3) / radiance + 1) *
            static_cast<float>(0.8) - 2.0f);
        require(std::abs(samples.values.view().at(0, 0) - expected) < 1e-5f);
        auto invalid = session.read_window(24, {0, 0, {1, 3}});
        require(invalid.valid.view().at(0, 1) == 0);
        calibrate(invalid, band(24).calibration, context);
        require(std::isnan(invalid.values.view().at(0, 1)));
        auto reflected = session.read_window(2, {1, 1, {1, 1}});
        calibrate(reflected, band(2).calibration, context);
        const float scaled = 21 * 3 + 1;
        require(reflected.values.view().at(0, 0) == 0.125f * scaled * scaled + 2 * scaled + 0.5f);
        auto solar = session.read_window(fy::sensors::solar_zenith, {0, 0, {1, 1}});
        calibrate(solar, band(fy::sensors::solar_zenith).calibration, context);
        require(solar.values.values()[0] == 60);
        auto geo = session.read_geometry("geo", {0, 0, {3, 5}});
        require(std::get<SwathGeometry>(geo).valid.view().at(0, 1) == 0);
        require(std::get<SwathGeometry>(geo).latitude.view().at(0, 1) == -9999.9f);
        require(band(1024).calibration.source == SourceRole::data);
        require(band(1024).source == SourceRole::auxiliary);
        require(band(1024).calibration.coefficients[3] == 0.8 && band(1024).calibration.coefficients[4] == -2);
    }
    {
        auto session = registry.open("mersi", {quarter, main, {}});
        require(session.metadata().resolution == "0250M");
        require(session.metadata().grid("native").shape == Shape{12,20});
    }
    {
        auto path = directory / "mersi-d.HDF";
        context.io().run([&] {
            mersi_file(path, false);
            H5::H5File file(path.string(), H5F_ACC_RDWR);
            auto satellite = file.openAttribute("Satellite Name");
            satellite.write(satellite.getDataType(), std::string("FY-3D"));
            hdf_fixture::numbers(file, "TBB_Trans_Coefficient_A", {1,1,1,1,0.6,1});
            hdf_fixture::numbers(file, "TBB_Trans_Coefficient_B", {0,0,0,0,4,0});
        });
        auto session = registry.open("mersi", {path, {}, {}});
        auto bands = session.bands();
        const auto found = std::ranges::find(bands, 24, &BandDescriptor::id);
        require(found != bands.end() && found->calibration.coefficients[2] == 933.364);
        require(found->calibration.coefficients[3] == 0.6 && found->calibration.coefficients[4] == 4);
    }
    for (int axis = 0; axis < 3; ++axis) {
        auto path = directory / ("mwri-axis" + std::to_string(axis) + ".HDF");
        context.io().run([&] { mwri_file(path, axis, false); });
        auto session = registry.open("mwri", {path, {}, {}});
        auto bands = session.bands();
        require(static_cast<int>(bands[8].channel_axis) == axis + 1);
        auto samples = session.read_window(8, {1, 2, {2, 2}});
        require(samples.values.view().at(1, 1) == 1823);
        calibrate(samples, bands[8].calibration, context);
        require(std::abs(samples.values.view().at(1, 1) - (1823 * 0.01f + 100)) < 1e-5f);
        auto invalid = session.read_window(8, {0,0,{1,2}});
        require(!invalid.valid.values()[0] && !invalid.valid.values()[1]);
        calibrate(invalid, bands[8].calibration, context);
        require(std::isnan(invalid.values.values()[0]) && std::isnan(invalid.values.values()[1]));
    }
    {
        auto path = directory / "mwri-h.HDF";
        context.io().run([&] { mwri_file(path, 2, true); });
        auto session = registry.open("mwri", {path, {}, {}});
        require(session.bands().size() == 22);
        auto sounding = session.read_window(111, {1, 2, {1, 1}});
        require(sounding.values.view().at(0, 0) == 2112);
    }
    {
        auto path = directory / "malformed.HDF";
        context.io().run([&] { mersi_file(path, false, true); });
        rejects([&] { registry.open("mersi", {path, {}, {}}); });
        auto session = registry.open("mersi", {main, {}, {}});
        require(session.metadata().satellite == "FY-3H");
    }
    {
        auto path = directory / "low-light.HDF";
        context.io().run([&] {
            H5::H5File file(path.string(), H5F_ACC_TRUNC);
            hdf_fixture::metadata(file, "FY-3E", "MERSI-LL");
            hdf_fixture::geolocation(file, "Geolocation", 2, 3);
            const hsize_t shape[]{2,3};
            const float values[]{1,2,3,4,5,6};
            auto dataset = hdf_fixture::dataset(file, "Data/EV_1KM_LL", shape, values);
            hdf_fixture::numbers(dataset, "Slope", {2});
            hdf_fixture::numbers(dataset, "Intercept", {1});
        });
        auto session = registry.open("mersi", {path,{}, {}});
        auto samples = session.read_window(fy::sensors::low_light, {1,1,{1,1}});
        calibrate(samples, session.bands().at(0).calibration, context);
        require(samples.values.values()[0] == 11);
    }
    const auto open = context.io().run([] {
        std::array<ssize_t,5> result;
        const unsigned kinds[]{H5F_OBJ_FILE,H5F_OBJ_DATASET,H5F_OBJ_GROUP,H5F_OBJ_ATTR,H5F_OBJ_DATATYPE};
        for (std::size_t i = 0; i < result.size(); ++i) result[i] = H5Fget_obj_count(H5F_OBJ_ALL,kinds[i]);
        return result;
    });
    for (std::size_t i = 0; i < open.size(); ++i)
        if (open[i] != (i == 4 ? predefined_types : 0))
            throw std::runtime_error("Open HDF kind " + std::to_string(i) + ": " + std::to_string(open[i]));
}
