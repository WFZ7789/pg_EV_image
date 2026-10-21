/// SPDX-License-Identifier: MIT

#include "sensors/hdf.hpp"
#include "products/config.hpp"

#include <H5Cpp.h>

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>

namespace fy::sensors {
namespace {

using namespace core;

struct Access {
    mutable std::optional<ProfileReport> report;

    void note(std::size_t logical, std::size_t decoded, std::size_t samples) const {
        if (report) report->record_io({logical, decoded, 1, samples});
    }

    std::string text(const H5::H5Object& object, const std::string& key, bool required = true) const {
        if (!object.attrExists(key)) {
            if (required) throw std::invalid_argument("Missing HDF attribute: " + key);
            return {};
        }
        auto attribute = object.openAttribute(key);
        if (attribute.getSpace().getSimpleExtentNpoints() != 1)
            throw std::invalid_argument("String attribute must be scalar: " + key);
        std::string value;
        attribute.read(attribute.getDataType(), value);
        note(value.size(), value.size(), 0);
        return value;
    }

    std::vector<double> attribute(const H5::H5Object& object, const std::string& key) const {
        if (!object.attrExists(key)) return {};
        auto source = object.openAttribute(key);
        auto count = source.getSpace().getSimpleExtentNpoints();
        if (count <= 0) throw std::invalid_argument("Empty HDF attribute: " + key);
        std::vector<double> values(static_cast<std::size_t>(count));
        source.read(H5::PredType::NATIVE_DOUBLE, values.data());
        note(values.size() * source.getDataType().getSize(), values.size() * sizeof(double), 0);
        return values;
    }

    double coefficient(const H5::H5Object& object, const std::string& key, std::size_t channel,
                       double fallback) const {
        auto values = attribute(object, key);
        if (values.empty()) return fallback;
        if (values.size() == 1) return values[0];
        if (channel >= values.size()) throw std::invalid_argument("Calibration attribute too short: " + key);
        return values[channel];
    }

    std::vector<double> fills(const H5::H5Object& object) const {
        std::vector<double> values;
        for (const auto* key : {"FillValue","_FillValue","Fill_Value"}) {
            auto source = attribute(object,key);
            values.insert(values.end(),source.begin(),source.end());
        }
        return values;
    }

    static bool exists(const H5::H5File& file, const std::string& path) {
        htri_t result;
        H5E_BEGIN_TRY { result = H5Lexists(file.getId(), path.c_str(), H5P_DEFAULT); } H5E_END_TRY;
        return result > 0;
    }

    static std::vector<hsize_t> dimensions(const H5::DataSet& dataset) {
        auto space = dataset.getSpace();
        const auto rank = space.getSimpleExtentNdims();
        if (rank < 1 || rank > 3) throw std::invalid_argument("Unsupported HDF dataset rank");
        std::vector<hsize_t> result(static_cast<std::size_t>(rank));
        space.getSimpleExtentDims(result.data());
        for (auto extent : result) if (!extent) throw std::invalid_argument("Empty HDF dataset");
        return result;
    }

    template<class T>
    std::vector<float> read_as(const H5::DataSet& dataset,const H5::DataType& memory_type,
                               const H5::DataSpace& memory,const H5::DataSpace& selection,
                               std::size_t size) const {
        std::vector<T> native(size);
        if (report) {
            auto phase = report->phase("hdf_read");
            dataset.read(native.data(),memory_type,memory,selection);
        } else dataset.read(native.data(),memory_type,memory,selection);
        std::vector<float> values;
        if constexpr (std::is_same_v<T,float>) values = std::move(native);
        else values.resize(size);
        if constexpr (!std::is_same_v<T,float>) {
            const auto convert = [&] {
                std::transform(native.begin(),native.end(),values.begin(),
                               [](T value) { return static_cast<float>(value); });
            };
            if (report) { auto phase = report->phase("hdf_convert"); convert(); }
            else convert();
        }
        return values;
    }

    std::vector<float> read(const H5::DataSet& dataset,const H5::DataSpace& memory,
                            const H5::DataSpace& selection,std::size_t size) const {
        const auto type = dataset.getDataType();
        std::vector<float> values;
        if (type.getClass() == H5T_INTEGER) {
            const H5::IntType integer(type.getId());
            const bool sign = integer.getSign() != H5T_SGN_NONE;
            switch (type.getSize()) {
                case 1: values = sign ? read_as<signed char>(dataset,H5::PredType::NATIVE_SCHAR,memory,selection,size)
                                      : read_as<unsigned char>(dataset,H5::PredType::NATIVE_UCHAR,memory,selection,size); break;
                case 2: values = sign ? read_as<short>(dataset,H5::PredType::NATIVE_SHORT,memory,selection,size)
                                      : read_as<unsigned short>(dataset,H5::PredType::NATIVE_USHORT,memory,selection,size); break;
                case 4: values = sign ? read_as<int>(dataset,H5::PredType::NATIVE_INT,memory,selection,size)
                                      : read_as<unsigned int>(dataset,H5::PredType::NATIVE_UINT,memory,selection,size); break;
                case 8: values = sign ? read_as<long long>(dataset,H5::PredType::NATIVE_LLONG,memory,selection,size)
                                      : read_as<unsigned long long>(dataset,H5::PredType::NATIVE_ULLONG,memory,selection,size); break;
                default: throw std::invalid_argument("Unsupported HDF integer width");
            }
        } else if (type.getClass() == H5T_FLOAT && type.getSize() == sizeof(float)) {
            values = read_as<float>(dataset,H5::PredType::NATIVE_FLOAT,memory,selection,size);
        } else if (type.getClass() == H5T_FLOAT && type.getSize() == sizeof(double)) {
            values = read_as<double>(dataset,H5::PredType::NATIVE_DOUBLE,memory,selection,size);
        } else throw std::invalid_argument("Unsupported HDF numeric type");
        note(size * type.getSize(),size * sizeof(float),size);
        return values;
    }

    std::vector<float> read(const H5::DataSet& dataset, std::span<const hsize_t> origin,
                            std::span<const hsize_t> count) const {
        if (origin.size() != count.size()) throw std::invalid_argument("Incorrect HDF window rank");
        auto dims = dimensions(dataset);
        if (dims.size() != count.size()) throw std::invalid_argument("Incorrect HDF window rank");
        std::size_t size = 1;
        for (std::size_t axis = 0; axis < dims.size(); ++axis) {
            if (!count[axis] || origin[axis] > dims[axis] || count[axis] > dims[axis] - origin[axis])
                throw std::out_of_range("HDF window outside dataset");
            if (count[axis] > std::numeric_limits<std::size_t>::max() / size)
                throw std::overflow_error("HDF window overflow");
            size *= count[axis];
        }
        auto selection = dataset.getSpace();
        selection.selectHyperslab(H5S_SELECT_SET, count.data(), origin.data());
        H5::DataSpace memory(static_cast<int>(count.size()), count.data());
        return read(dataset,memory,selection,size);
    }
};

struct Field {
    BandDescriptor band;
    int channel_axis = -1;
    bool flat_spatial = false;
};

struct Coordinates {
    SourceRole source;
    std::string latitude;
    std::string longitude;
    std::vector<double> lat_fill;
    std::vector<double> lon_fill;
    float lat_slope = 1, lat_intercept = 0;
    float lon_slope = 1, lon_intercept = 0;
    int channel_axis = -1;
    bool flat_spatial = false;
};

bool filled(float value, std::span<const double> fill_values) {
    return std::ranges::any_of(fill_values, [value](double fill) {
        return value == static_cast<float>(fill) || (std::isnan(value) && std::isnan(fill));
    });
}

class HdfAdapter final : public SensorAdapter {
public:
    HdfAdapter(const SourceFiles& sources, std::string kind, std::optional<ProfileReport> report)
        : access_{std::move(report)} {
        files_[0] = std::make_unique<H5::H5File>(sources.data.string(), H5F_ACC_RDONLY);
        const auto root = file(SourceRole::data).openGroup("/");
        metadata_.satellite = access_.text(root, "Satellite Name");
        metadata_.sensor = access_.text(root, "Sensor Identification Code", kind != "pmr");
        if (kind == "pmr") {
            if (metadata_.satellite.rfind("FY-3",0) != 0 ||
                (!metadata_.sensor.empty() && metadata_.sensor.find("PMR") == std::string::npos))
                throw std::invalid_argument("PMR adapter received a different sensor");
            if (metadata_.sensor.empty()) metadata_.sensor = "PMR";
        }
        const auto alias = access_.text(root, "File Alias Name", false);
        const auto date = access_.text(root, "Observing Beginning Date");
        const auto time = access_.text(root, "Observing Beginning Time");
        metadata_.observation_begin = date + "T" + time;
        const auto distance_attribute = kind == "fy4" ?
            (metadata_.sensor == "GHI" ? "Earth_Sun Distance Ratio" : "Earth/Sun Distance Ratio") : "EarthSun Distance Ratio";
        auto distance = access_.attribute(root, distance_attribute);
        if (!distance.empty()) {
            if (distance.size() != 1 || !std::isfinite(distance[0]) || distance[0] <= 0)
                throw std::invalid_argument("Invalid Earth-Sun distance ratio");
            metadata_.numeric_attributes.emplace("earth_sun_distance_au", distance[0]);
        }
        const auto ending_date = access_.text(root, "Observing Ending Date", false);
        const auto ending_time = access_.text(root, "Observing Ending Time", false);
        if (!ending_date.empty() && !ending_time.empty()) metadata_.observation_end = ending_date + "T" + ending_time;
        metadata_.labels = {access_.text(root, "File Name", false), date, time + "(UTC)",
                            metadata_.satellite + " " + metadata_.sensor, ""};
        if (metadata_.labels[0].empty()) metadata_.labels[0] = sources.data.filename().string();
        if (kind == "mersi" && metadata_.sensor.find("MERSI") == std::string::npos)
            throw std::invalid_argument("MERSI adapter received a different sensor");
        if (kind == "mwri" && metadata_.sensor.find("MWRI") == std::string::npos)
            throw std::invalid_argument("MWRI adapter received a different sensor");
        if (kind == "fy4" && (metadata_.satellite.rfind("FY-4",0) != 0 ||
                             (metadata_.sensor != "AGRI" && metadata_.sensor != "GHI")))
            throw std::invalid_argument("FY4 adapter requires AGRI or GHI");
        if (kind == "giirs" && (metadata_.satellite.rfind("FY-4",0) != 0 || metadata_.sensor != "GIIRS"))
            throw std::invalid_argument("GIIRS adapter received a different sensor");
        if (!sources.geolocation.empty() && sources.geolocation != sources.data) {
            files_[1] = std::make_unique<H5::H5File>(sources.geolocation.string(), H5F_ACC_RDONLY);
            validate_pair(*files_[1], sources.data, sources.geolocation);
        }
        if (kind == "mersi") {
            const bool quarter = alias.find("250M") != std::string::npos || alias.find("QKM") != std::string::npos;
            metadata_.resolution = quarter ? "0250M" : alias.find("500M") != std::string::npos ? "0500M" : "1000M";
            const auto filename = sources.data.filename().string();
            std::smatch resolution;
            if (std::regex_search(filename, resolution, std::regex(R"(_([0-9]{4}M)_)")))
                metadata_.resolution = resolution[1].str();
            if (quarter && metadata_.resolution != "0250M")
                throw std::invalid_argument("MERSI alias and filename resolution mismatch");
            add_coordinates("geo", SourceRole::geolocation, "Geolocation", quarter || !sources.auxiliary.empty());
            add_mersi_grid("native", SourceRole::data, quarter, 0);
            if (!sources.auxiliary.empty()) {
                files_[2] = std::make_unique<H5::H5File>(sources.auxiliary.string(), H5F_ACC_RDONLY);
                validate_pair(*files_[2], sources.data, sources.auxiliary);
                auto aux_alias = access_.text(*files_[2], "File Alias Name");
                if (aux_alias.find("250M") == std::string::npos && aux_alias.find("QKM") == std::string::npos)
                    throw std::invalid_argument("MERSI auxiliary file must have 250M resolution");
                add_mersi_grid("auxiliary", SourceRole::auxiliary, true, auxiliary_band_offset);
            }
            add_angle(solar_zenith, "Geolocation/SolarZenith");
            add_angle(solar_azimuth, "Geolocation/SolarAzimuth");
            add_angle(satellite_zenith, "Geolocation/SensorZenith");
            add_angle(satellite_azimuth, "Geolocation/SensorAzimuth");
        } else if (kind == "fy4") {
            std::smatch resolution;
            if (!std::regex_search(alias,resolution,std::regex(R"(([0-9]{4}M))")))
                throw std::invalid_argument("Missing FY4 resolution in file alias");
            metadata_.resolution = resolution[1].str();
            add_fy4_grid("native",SourceRole::data,0);
            if (!sources.auxiliary.empty()) {
                files_[2] = std::make_unique<H5::H5File>(sources.auxiliary.string(),H5F_ACC_RDONLY);
                validate_pair(*files_[2],sources.data,sources.auxiliary);
                add_fy4_grid("auxiliary",SourceRole::auxiliary,auxiliary_band_offset);
            }
            add_fy4_angles();
        } else if (kind == "giirs") {
            metadata_.resolution = "native";
            std::smatch resolution;
            const auto filename = sources.data.filename().string();
            if (std::regex_search(filename,resolution,std::regex(R"(_([0-9]{3}KM)_)")))
                metadata_.resolution = resolution[1].str();
            const auto extent = [&](const std::string& axis) -> std::size_t {
                const auto begin = required_scalar(root,"Begin " + axis + " Number");
                const auto end = required_scalar(root,"End " + axis + " Number");
                if (begin < 0 || end < begin || std::floor(begin) != begin || std::floor(end) != end || end > 65535)
                    throw std::invalid_argument("Invalid GIIRS observation dimensions");
                return static_cast<std::size_t>(end - begin + 1);
            };
            const Shape observations{extent("Pixel"),extent("Line")};
            for (const auto& [family,offset] : {std::pair{"LW",giirs_lw_offset},std::pair{"MW",giirs_mw_offset}})
                if (Access::exists(file(SourceRole::data),std::string("Data/ES_Real") + family))
                    add_giirs_spectrum(family,offset,observations);
            if (Access::exists(file(SourceRole::data),"Data/VIS_DN")) {
                const auto dims = Access::dimensions(file(SourceRole::data).openDataSet("Data/VIS_DN"));
                if (dims.size() != 2) throw std::invalid_argument("Invalid GIIRS visible dimensions");
                add_giirs_coordinates("VIS",{dims[0],dims[1]});
                auto field = describe(1,SourceRole::data,"Data/VIS_DN","VIS",-1,0);
                field.band.calibrated_unit = "DN";
                fields_.emplace(1,std::move(field));
                const auto table = Access::dimensions(file(SourceRole::data).openDataSet("Data/VIS_CalTable"));
                if ((table.size() != 3 && table.size() != 2) || table.back() != 3)
                    throw std::invalid_argument("Invalid GIIRS visible coefficient dimensions");
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    auto coefficient = describe(static_cast<int>(channel) + 2,SourceRole::data,"Data/VIS_CalTable","VIS",
                                                static_cast<int>(table.size() - 1),channel,true,table.size() == 2);
                    coefficient.band.calibrated_unit = "1";
                    coefficient.band.valid_range.reset();
                    fields_.emplace(coefficient.band.id,std::move(coefficient));
                }
            }
            if (fields_.empty()) throw std::invalid_argument("GIIRS file has no supported datasets");
        } else if (kind == "pmr") {
            if (!Access::exists(file(SourceRole::data),"SLV/zFactorCorrectedESurface") ||
                !Access::exists(file(SourceRole::geolocation),"Geo_Fields/Latitude") ||
                !Access::exists(file(SourceRole::geolocation),"Geo_Fields/Longitude"))
                throw std::invalid_argument("PMR requires Geo_Fields/SLV datasets");
            std::smatch resolution;
            const auto filename = sources.data.filename().string();
            metadata_.resolution = std::regex_search(filename,resolution,std::regex(R"(_([0-9]{4}M)_)")) ?
                                   resolution[1].str() : "native";
            metadata_.labels[3] = metadata_.satellite + " " + alias;
            add_coordinates("native",SourceRole::geolocation,"Geo_Fields",true,2);
            auto field = describe(1,SourceRole::data,"SLV/zFactorCorrectedESurface","native",-1,0);
            auto dataset = file(SourceRole::data).openDataSet(field.band.dataset);
            const auto name = access_.text(dataset,"Long_Name",false);
            if (!name.empty()) field.band.name = name;
            field.band.calibrated_unit = "dBZ";
            fields_.emplace(1,std::move(field));
        } else if (kind == "mwri") {
            metadata_.resolution = "010KM";
            if (metadata_.satellite == "FY-3D") {
                add_coordinates("window", SourceRole::geolocation, "Geolocation", false);
                add_mwri("window", "Calibration/EARTH_OBSERVE_BT_10_to_89GHz", 10, 0, true);
            } else if (metadata_.satellite == "FY-3H") {
                add_coordinates("window", SourceRole::geolocation, "Window Channel/Geolocation", false);
                add_mwri("window", "Window Channel/Calibration/EARTH_OBSERVE_BT", 10, 0, false);
                add_coordinates("sounding", SourceRole::geolocation, "Sounding Channel/Geolocation", false);
                add_mwri("sounding", "Sounding Channel/Calibration/EARTH_OBSERVE_BT", 12,
                         sounding_band_offset, false);
            } else throw std::invalid_argument("Unsupported MWRI satellite: " + metadata_.satellite);
        } else throw std::invalid_argument("Unknown HDF sensor adapter");
        metadata_.validate();
    }

    SensorMetadata metadata() const override { return metadata_; }

    std::vector<BandDescriptor> bands() const override {
        std::vector<BandDescriptor> result;
        for (const auto& [id, field] : fields_) result.push_back(field.band);
        return result;
    }

    std::size_t preferred_block_rows(int band,Window window) override {
        const auto& field = fields_.at(band);
        auto& dataset = open_dataset(field.band.source,field.band.dataset);
        const auto& dimensions = dataset_info(field.band.source,field.band.dataset).dimensions;
        if (field.flat_spatial) return window.shape.rows;
        const auto properties = dataset.getCreatePlist();
        if (properties.getLayout() != H5D_CHUNKED) return window.shape.rows;
        std::vector<hsize_t> chunk(dimensions.size());
        properties.getChunk(static_cast<int>(chunk.size()),chunk.data());
        std::size_t spatial_axis = 0;
        while (static_cast<int>(spatial_axis) == field.channel_axis) ++spatial_axis;
        return std::min<std::size_t>(window.shape.rows,std::max<hsize_t>(256,chunk.at(spatial_axis)));
    }

    SampleBlock read_window(int band, Window window) override try {
        const auto& field = fields_.at(band);
        const auto& descriptor = field.band;
        window.validate(metadata_.grid(descriptor.grid).shape);
        auto& dataset = open_dataset(descriptor.source,descriptor.dataset);
        const auto& dims = dataset_info(descriptor.source,descriptor.dataset).dimensions;
        std::vector<float> raw;
        if (field.flat_spatial) {
            const auto spatial = 1 - field.channel_axis;
            const auto shape = metadata_.grid(descriptor.grid).shape;
            hsize_t origin[]{0,0},stride[]{1,1},count[]{1,1},block[]{1,1};
            origin[field.channel_axis] = descriptor.channel_index;
            origin[spatial] = window.row * shape.cols + window.col;
            stride[spatial] = shape.cols; count[spatial] = window.shape.rows; block[spatial] = window.shape.cols;
            auto selection = dataset.getSpace();
            selection.selectHyperslab(H5S_SELECT_SET,count,origin,stride,block);
            const hsize_t memory_shape[]{window.shape.rows,window.shape.cols};
            H5::DataSpace memory(2,memory_shape);
            raw = access_.read(dataset,memory,selection,window.shape.elements());
        } else {
            std::vector<hsize_t> origin(dims.size(), 0), count = dims;
            std::size_t spatial_axis = 0;
            for (std::size_t axis = 0; axis < dims.size(); ++axis) {
                if (static_cast<int>(axis) == field.channel_axis) {
                    origin[axis] = descriptor.channel_index;
                    count[axis] = 1;
                } else {
                    origin[axis] = spatial_axis == 0 ? window.row : window.col;
                    count[axis] = spatial_axis == 0 ? window.shape.rows : window.shape.cols;
                    ++spatial_axis;
                }
            }
            raw = access_.read(dataset,origin,count);
        }
        std::vector<std::uint8_t> raw_valid(raw.size());
        const bool integer_source = dataset.getDataType().getClass() == H5T_INTEGER;
        const auto validate = [&] {
            std::vector<float> excluded;
            for (double fill : descriptor.fill_values)
                if (std::isfinite(fill) && (!descriptor.valid_range ||
                    (fill >= (*descriptor.valid_range)[0] && fill <= (*descriptor.valid_range)[1])))
                    excluded.push_back(static_cast<float>(fill));
            if (descriptor.valid_range && excluded.empty()) {
                const auto [low,high] = *descriptor.valid_range;
                if (integer_source)
                    for (std::size_t i = 0; i < raw.size(); ++i)
                        raw_valid[i] = raw[i] >= low && raw[i] <= high;
                else
                    for (std::size_t i = 0; i < raw.size(); ++i)
                        raw_valid[i] = std::isfinite(raw[i]) && raw[i] >= low && raw[i] <= high;
            } else if (descriptor.valid_range && excluded.size() == 1) {
                const auto [low,high] = *descriptor.valid_range;
                const float fill = excluded.front();
                if (integer_source)
                    for (std::size_t i = 0; i < raw.size(); ++i)
                        raw_valid[i] = raw[i] >= low && raw[i] <= high && raw[i] != fill;
                else
                    for (std::size_t i = 0; i < raw.size(); ++i)
                        raw_valid[i] = std::isfinite(raw[i]) && raw[i] >= low && raw[i] <= high && raw[i] != fill;
            } else if (!descriptor.valid_range && excluded.empty()) {
                if (integer_source) std::fill(raw_valid.begin(),raw_valid.end(),std::uint8_t{1});
                else for (std::size_t i = 0; i < raw.size(); ++i) raw_valid[i] = std::isfinite(raw[i]);
            } else if (!descriptor.valid_range && excluded.size() == 1) {
                const float fill = excluded.front();
                if (integer_source)
                    for (std::size_t i = 0; i < raw.size(); ++i) raw_valid[i] = raw[i] != fill;
                else
                    for (std::size_t i = 0; i < raw.size(); ++i)
                        raw_valid[i] = std::isfinite(raw[i]) && raw[i] != fill;
            } else {
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    const float value = raw[i];
                    raw_valid[i] = std::isfinite(value) && !filled(value,descriptor.fill_values) &&
                        (!descriptor.valid_range ||
                         (value >= (*descriptor.valid_range)[0] && value <= (*descriptor.valid_range)[1]));
                }
            }
        };
        if (access_.report) { auto phase = access_.report->phase("hdf_validate"); validate(); }
        else validate();
        Raster<float> values(window.shape, std::move(raw));
        Raster<std::uint8_t> valid(window.shape,std::move(raw_valid));
        return {band, window, std::move(values), std::move(valid)};
    } catch (const H5::Exception& error) {
        throw std::runtime_error("HDF5 window read: " + error.getDetailMsg());
    }

    NativeGeometry read_geometry(const std::string& grid, Window window) override try {
        if (auto disk = disks_.find(grid); disk != disks_.end()) {
            window.validate(disk->second.shape);
            auto geometry = disk->second;
            geometry.shape = window.shape;
            geometry.scan_origin_rad[0] += window.col * geometry.scan_step_rad[0];
            geometry.scan_origin_rad[1] += window.row * geometry.scan_step_rad[1];
            return geometry;
        }
        if (images_.contains(grid)) {
            window.validate(metadata_.grid(grid).shape);
            return ImageGeometry{window.shape};
        }
        if (!coordinates_.contains(grid)) throw std::invalid_argument("No native navigation for grid: " + grid);
        const auto& coordinates = coordinates_.at(grid);
        window.validate(metadata_.grid(grid).shape);
        std::vector<hsize_t> origin{window.row,window.col}, count{window.shape.rows,window.shape.cols};
        if (coordinates.channel_axis == 2) { origin.push_back(0); count.push_back(1); }
        auto& latitude = open_dataset(coordinates.source,coordinates.latitude);
        auto& longitude = open_dataset(coordinates.source,coordinates.longitude);
        const auto read = [&](const H5::DataSet& dataset) {
            if (!coordinates.flat_spatial) return access_.read(dataset,origin,count);
            const auto shape = metadata_.grid(grid).shape;
            const hsize_t start[]{window.row * shape.cols + window.col},stride[]{shape.cols},
                          blocks[]{window.shape.rows},width[]{window.shape.cols},memory_shape[]{window.shape.rows,window.shape.cols};
            auto selection = dataset.getSpace();
            selection.selectHyperslab(H5S_SELECT_SET,blocks,start,stride,width);
            H5::DataSpace memory(2,memory_shape);
            return access_.read(dataset,memory,selection,window.shape.elements());
        };
        auto lat = read(latitude), lon = read(longitude);
        Raster<std::uint8_t> valid(window.shape);
        for (std::size_t i = 0; i < lat.size(); ++i) {
            bool raw_valid = !filled(lat[i], coordinates.lat_fill) && !filled(lon[i], coordinates.lon_fill);
            lat[i] = lat[i] * coordinates.lat_slope + coordinates.lat_intercept;
            lon[i] = lon[i] * coordinates.lon_slope + coordinates.lon_intercept;
            valid.values()[i] = raw_valid && std::isfinite(lat[i]) && std::isfinite(lon[i]) &&
                lat[i] >= -90 && lat[i] <= 90 && lon[i] >= -180 && lon[i] <= 360;
        }
        return SwathGeometry{Raster<float>(window.shape, std::move(lat)),
                             Raster<float>(window.shape, std::move(lon)), std::move(valid)};
    } catch (const H5::Exception& error) {
        throw std::runtime_error("HDF5 geometry read: " + error.getDetailMsg());
    }

private:
    struct DatasetInfo {
        std::vector<hsize_t> dimensions;
        std::string name;
        std::vector<double> fills,range,slope,intercept;
    };

    Access access_;
    std::array<std::unique_ptr<H5::H5File>, 3> files_;
    SensorMetadata metadata_;
    std::map<int, Field> fields_;
    std::map<std::string, Coordinates> coordinates_;
    std::map<std::string, NativeDiskGeometry> disks_;
    std::set<std::string> images_;
    std::map<std::pair<SourceRole,std::string>,H5::DataSet> datasets_;
    std::map<std::pair<SourceRole,std::string>,DatasetInfo> dataset_info_;

    H5::H5File& file(SourceRole source) const {
        const auto index = static_cast<std::size_t>(source);
        if (files_[index]) return *files_[index];
        if (source == SourceRole::geolocation) return *files_[0];
        throw std::invalid_argument("Missing HDF source file");
    }

    H5::DataSet& open_dataset(SourceRole source,const std::string& path) {
        auto key = std::pair{source,path};
        auto found = datasets_.find(key);
        if (found == datasets_.end())
            found = datasets_.emplace(std::move(key),file(source).openDataSet(path)).first;
        return found->second;
    }

    const DatasetInfo& dataset_info(SourceRole source,const std::string& path) {
        auto key = std::pair{source,path};
        auto found = dataset_info_.find(key);
        if (found != dataset_info_.end()) return found->second;
        auto& dataset = open_dataset(source,path);
        DatasetInfo result;
        result.dimensions = Access::dimensions(dataset);
        result.name = access_.text(dataset,"long_name",false);
        result.fills = access_.fills(dataset);
        result.range = access_.attribute(dataset,"valid_range");
        if (result.range.empty() && dataset.attrExists("Valid_Range") &&
            dataset.openAttribute("Valid_Range").getSpace().getSimpleExtentNpoints() > 0)
            result.range = access_.attribute(dataset,"Valid_Range");
        result.slope = access_.attribute(dataset,"Slope");
        result.intercept = access_.attribute(dataset,"Intercept");
        return dataset_info_.emplace(std::move(key),std::move(result)).first->second;
    }

    void validate_pair(const H5::H5File& other, const std::filesystem::path& data,
                        const std::filesystem::path& paired) const {
        const auto root = other.openGroup("/");
        for (auto [key, expected] : {std::pair{"Satellite Name", metadata_.satellite},
                                     {"Sensor Identification Code", metadata_.sensor}}) {
            auto value = access_.text(root, key, false);
            if (!value.empty() && value != expected) throw std::invalid_argument("Paired HDF sensor mismatch");
        }
        auto date = access_.text(root, "Observing Beginning Date", false);
        auto time = access_.text(root, "Observing Beginning Time", false);
        if (!date.empty() && !time.empty() && date + "T" + time != metadata_.observation_begin)
            throw std::invalid_argument("Paired HDF observation mismatch");
        const std::regex pattern(R"((20[0-9]{6})_([0-9]{4}))");
        std::smatch a, b;
        const auto first = data.filename().string(), second = paired.filename().string();
        if (std::regex_search(first, a, pattern) && std::regex_search(second, b, pattern) && a.str() != b.str())
            throw std::invalid_argument("Paired HDF filename time mismatch");
    }

    void add_coordinates(std::string id, SourceRole source, const std::string& group, bool scaled,
                         int channel_axis = -1) {
        auto lat = file(source).openDataSet(group + "/Latitude");
        auto lon = file(source).openDataSet(group + "/Longitude");
        auto dims = Access::dimensions(lat);
        if ((dims.size() != 2 && !(channel_axis == 2 && dims.size() == 3)) || dims != Access::dimensions(lon))
            throw std::invalid_argument("Geolocation shapes do not match");
        metadata_.grids.push_back({.id = id, .shape = {dims[0], dims[1]}, .source = source});
        Coordinates coordinates{source, group + "/Latitude", group + "/Longitude",
                                access_.fills(lat), access_.fills(lon)};
        coordinates.channel_axis = dims.size() == 3 ? channel_axis : -1;
        if (scaled) {
            coordinates.lat_slope = static_cast<float>(access_.coefficient(lat, "Slope", 0, 1));
            coordinates.lat_intercept = static_cast<float>(access_.coefficient(lat, "Intercept", 0, 0));
            coordinates.lon_slope = static_cast<float>(access_.coefficient(lon, "Slope", 0, 1));
            coordinates.lon_intercept = static_cast<float>(access_.coefficient(lon, "Intercept", 0, 0));
        }
        coordinates_.emplace(std::move(id), std::move(coordinates));
    }

    Field describe(int id, SourceRole source, std::string path, const std::string& grid,
                   int axis, std::size_t channel, bool linear = true,bool flat_spatial = false) {
        const auto& cached = dataset_info(source,path);
        const auto& dims = cached.dimensions;
        if (flat_spatial && (dims.size() != 2 || axis < 0 || axis > 1))
            throw std::invalid_argument("Flattened observation grid requires two dataset axes");
        std::vector<std::size_t> shape;
        for (std::size_t i = 0; i < dims.size(); ++i)
            if (static_cast<int>(i) != axis) shape.push_back(dims[i]);
        const auto expected = metadata_.grid(grid).shape;
        if ((flat_spatial ? shape.size() != 1 || shape[0] != expected.elements() :
                           shape.size() != 2 || Shape{shape[0],shape[1]} != expected) ||
            (axis >= 0 && channel >= dims.at(static_cast<std::size_t>(axis))))
            throw std::invalid_argument("Band and grid shape mismatch: " + path);
        Field field;
        field.channel_axis = axis;
        field.flat_spatial = flat_spatial;
        auto& band = field.band;
        band.id = id; band.source = source; band.dataset = std::move(path); band.grid = grid;
        band.unit = "DN";
        band.name = cached.name;
        band.channel_index = channel;
        band.channel_axis = axis < 0 ? ChannelAxis::none : axis == 0 ? ChannelAxis::first :
                            static_cast<std::size_t>(axis) + 1 == dims.size() ? ChannelAxis::last : ChannelAxis::middle;
        band.fill_values = cached.fills;
        const auto& range = cached.range;
        if (!range.empty()) {
            if (range.size() != 2 || range[0] > range[1]) throw std::invalid_argument("Invalid HDF valid range");
            band.valid_range = std::array<double, 2>{range[0], range[1]};
        }
        band.calibration.source = source;
        if (linear) {
            band.calibration.kind = CalibrationKind::linear;
            band.calibration.provenance = band.dataset + " attributes Slope/Intercept";
            if (cached.slope.empty() || cached.intercept.empty())
                throw std::invalid_argument("Missing band calibration attributes: " + band.dataset);
            const auto coefficient = [channel](const std::vector<double>& values,const std::string& key) {
                if (values.size() == 1) return values[0];
                if (channel >= values.size()) throw std::invalid_argument("Calibration attribute too short: " + key);
                return values[channel];
            };
            band.calibration.coefficients = {coefficient(cached.slope,"Slope"),coefficient(cached.intercept,"Intercept")};
        }
        return field;
    }

    void add_giirs_coordinates(const std::string& grid,Shape shape) {
        const auto latitude = "Geolocation/Latitude_" + grid,longitude = "Geolocation/Longitude_" + grid;
        auto lat = file(SourceRole::geolocation).openDataSet(latitude),lon = file(SourceRole::geolocation).openDataSet(longitude);
        const auto dims = Access::dimensions(lat);
        if (dims != Access::dimensions(lon) ||
            (dims.size() == 1 ? dims[0] != shape.elements() :
             dims.size() != 2 || Shape{dims[0],dims[1]} != shape))
            throw std::invalid_argument("GIIRS observation and geolocation shapes do not match");
        metadata_.grids.push_back({.id = grid,.shape = shape,.source = SourceRole::geolocation});
        Coordinates coordinates{SourceRole::geolocation,latitude,longitude,access_.fills(lat),access_.fills(lon)};
        coordinates.flat_spatial = dims.size() == 1;
        coordinates_.emplace(grid,std::move(coordinates));
    }

    void add_giirs_spectrum(const std::string& family,int offset,Shape observations) {
        add_giirs_coordinates(family,observations);
        const auto path = "Data/ES_Real" + family;
        auto dataset = file(SourceRole::data).openDataSet(path);
        const auto dims = Access::dimensions(dataset);
        if (dims.size() != 2 || dims[1] != observations.elements() || dims[0] >= giirs_lw_offset)
            throw std::invalid_argument("Invalid GIIRS spectral dimensions");
        auto wn = file(SourceRole::data).openDataSet("Data/WN_" + family);
        const auto wn_dims = Access::dimensions(wn);
        if (wn_dims.size() != 1 || wn_dims[0] != dims[0])
            throw std::invalid_argument("GIIRS wavenumber count differs from its spectrum");
        const hsize_t origin[]{0},count[]{dims[0]};
        auto wavenumbers = access_.read(wn,origin,count);
        const auto slope = static_cast<float>(required_scalar(wn,"Slope"));
        const auto intercept = static_cast<float>(required_scalar(wn,"Intercept"));
        const auto fill = access_.fills(wn);
        for (std::size_t channel = 0; channel < wavenumbers.size(); ++channel) {
            const auto raw = wavenumbers[channel];
            const float wavenumber = raw * slope + intercept;
            if (filled(raw,fill) || !std::isfinite(wavenumber) || wavenumber <= 0)
                throw std::invalid_argument("Invalid GIIRS wavenumber");
            metadata_.numeric_attributes.emplace("wavenumber." + family + "." + std::to_string(channel),wavenumber);
            auto field = describe(offset + static_cast<int>(channel),SourceRole::data,path,family,0,channel,true,true);
            field.band.unit = "mW/(m2 sr cm-1)";
            field.band.calibrated_unit = field.band.unit;
            fields_.emplace(field.band.id,std::move(field));
        }
    }

    double required_scalar(const H5::H5Object& object, const std::string& key) const {
        auto values = access_.attribute(object,key);
        if (values.size() != 1 || !std::isfinite(values[0]))
            throw std::invalid_argument("Invalid FY4 scalar attribute: " + key);
        return values[0];
    }

    void add_fy4_navigation(const std::string& grid, SourceRole source, Shape shape) {
        const auto& root = file(source);
        for (auto key : {"NOMCenterLat","NOMCenterLon","NOMSatHeight","dEA","dObRecFlat","dSamplingAngle","dSteppingAngle"})
            if (!root.attrExists(key)) return;
        if (required_scalar(root,"NOMCenterLat") != 0 || shape.rows != shape.cols ||
            required_scalar(root,"Begin Line Number") != 0 || required_scalar(root,"Begin Pixel Number") != 0) return;
        NativeDiskGeometry geometry;
        geometry.shape = shape;
        geometry.longitude_deg = required_scalar(root,"NOMCenterLon");
        geometry.equatorial_radius_m = required_scalar(root,"dEA");
        const auto inverse_flattening = required_scalar(root,"dObRecFlat");
        if (inverse_flattening <= 1) throw std::invalid_argument("Invalid FY4 inverse flattening");
        geometry.polar_radius_m = geometry.equatorial_radius_m * (1 - 1 / inverse_flattening);
        geometry.satellite_distance_m = required_scalar(root,"NOMSatHeight") + geometry.equatorial_radius_m;
        geometry.scan_step_rad = {required_scalar(root,"dSamplingAngle") * 1e-6,
                                  -required_scalar(root,"dSteppingAngle") * 1e-6};
        geometry.scan_origin_rad = {-(double(shape.cols) - 1) / 2 * geometry.scan_step_rad[0],
                                   -(double(shape.rows) - 1) / 2 * geometry.scan_step_rad[1]};
        geometry.validate();
        disks_.emplace(grid,geometry);
    }

    void add_fy4_grid(const std::string& grid, SourceRole source, int id_offset) {
        const auto& root = file(source);
        auto extent = [&](const char* first, const char* last) {
            const auto begin = required_scalar(root,first), end = required_scalar(root,last);
            if (begin < 0 || end < begin || end > 65535 || std::floor(begin) != begin || std::floor(end) != end)
                throw std::invalid_argument("Invalid FY4 image extent");
            return static_cast<std::size_t>(end - begin + 1);
        };
        Shape shape{extent("Begin Line Number","End Line Number"),extent("Begin Pixel Number","End Pixel Number")};
        metadata_.grids.push_back({.id = grid,.shape = shape,.source = source});
        auto group = root.openGroup("Data");
        std::size_t found = 0;
        for (hsize_t index = 0; index < group.getNumObjs(); ++index) {
            const auto name = group.getObjnameByIdx(index);
            std::smatch match;
            if (!std::regex_match(name,match,std::regex(R"(NOMChannel([0-9]{2}))"))) continue;
            const auto channel = std::stoi(match[1].str());
            if (!channel) throw std::invalid_argument("Invalid FY4 channel number");
            auto field = describe(channel + id_offset,source,"Data/" + name,grid,-1,0,false);
            const auto path = "Calibration/CALChannel" + match[1].str();
            auto table = root.openDataSet(path);
            const auto dims = Access::dimensions(table);
            if (dims.size() != 1) throw std::invalid_argument("FY4 calibration table must be one-dimensional");
            const hsize_t origin[]{0};
            auto coefficients = access_.read(table,origin,dims);
            auto& band = field.band;
            band.calibration.kind = CalibrationKind::lookup;
            band.calibration.provenance = path;
            band.calibration.fill_values = access_.attribute(table,"FillValue");
            band.calibration.coefficients.reserve(coefficients.size());
            band.calibration.coefficients.assign(coefficients.begin(),coefficients.end());
            const auto maximum = double(coefficients.size() - 1);
            if (band.valid_range) {
                (*band.valid_range)[0] = std::max(0.0,(*band.valid_range)[0]);
                (*band.valid_range)[1] = std::min(maximum,(*band.valid_range)[1]);
                if ((*band.valid_range)[0] > (*band.valid_range)[1])
                    throw std::invalid_argument("FY4 DN range does not intersect calibration table");
            } else band.valid_range = std::array<double,2>{0,maximum};
            band.calibrated_unit = channel <= 6 ? "1" : "K";
            fields_.emplace(band.id,std::move(field));
            ++found;
        }
        if (!found) throw std::invalid_argument("No FY4 NOM channels found");
        add_fy4_navigation(grid,source,shape);
        if (!disks_.contains(grid)) images_.insert(grid);
    }

    void add_fy4_angles() {
        const std::array fields{std::pair{solar_zenith,"Navigation/NOMSunZenith"},
                                std::pair{solar_azimuth,"Navigation/NOMSunAzimuth"},
                                std::pair{satellite_zenith,"Navigation/NOMSatelliteZenith"}};
        bool initialized = false;
        for (const auto& [id,path] : fields) {
            if (!Access::exists(file(SourceRole::geolocation),path)) continue;
            if (!initialized) {
                auto dataset = file(SourceRole::geolocation).openDataSet(path);
                auto dims = Access::dimensions(dataset);
                if (dims.size() != 2) throw std::invalid_argument("Invalid FY4 angle grid");
                metadata_.grids.push_back({.id = "angles",.shape = {dims[0],dims[1]},.source = SourceRole::geolocation});
                initialized = true;
            }
            auto field = describe(id,SourceRole::geolocation,path,"angles",-1,0);
            field.band.calibrated_unit = "degree";
            fields_.emplace(id,std::move(field));
        }
    }

    void add_mersi_grid(const std::string& grid, SourceRole source, bool quarter, int id_offset) {
        const auto& satellite = kSatConfigs.at(metadata_.satellite);
        Shape shape;
        for (const auto& [id, band] : satellite.bands) {
            const auto name = quarter ? band.qkdataset : band.akdataset;
            if (name.empty() || name == "EV_1KM_LL" || !Access::exists(file(source), "Data/" + name)) continue;
            auto dataset = file(source).openDataSet("Data/" + name);
            auto dims = Access::dimensions(dataset);
            if (dims.size() != (quarter ? 2 : 3)) throw std::invalid_argument("Invalid MERSI band rank");
            Shape candidate{dims[dims.size() - 2], dims.back()};
            if (shape == Shape{}) shape = candidate;
            else if (shape != candidate) throw std::invalid_argument("MERSI native bands have different grids");
        }
        if (shape == Shape{} && !quarter && Access::exists(file(source), "Data/EV_1KM_LL")) {
            auto low_light = file(source).openDataSet("Data/EV_1KM_LL");
            auto dims = Access::dimensions(low_light);
            if (dims.size() != 2) throw std::invalid_argument("Invalid MERSI low-light shape");
            shape = {dims[0], dims[1]};
        }
        if (!shape.elements()) throw std::invalid_argument("No supported MERSI channels found");
        const auto geo_shape = metadata_.grid("geo").shape;
        const std::array<double, 2> scale{double(geo_shape.rows) / shape.rows, double(geo_shape.cols) / shape.cols};
        metadata_.grids.push_back({grid, shape, source, "geo", scale, {(scale[0] - 1) / 2, (scale[1] - 1) / 2}});
        for (const auto& [id, config] : satellite.bands) {
            const auto name = quarter ? config.qkdataset : config.akdataset;
            if (name.empty() || name == "EV_1KM_LL" || !Access::exists(file(source), "Data/" + name)) continue;
            auto dataset = file(source).openDataSet("Data/" + name);
            auto dims = Access::dimensions(dataset);
            if (!quarter && config.akid >= dims.front()) continue;
            auto field = describe(id + id_offset, source, "Data/" + name, grid, quarter ? -1 : 0,
                                  quarter ? 0 : config.akid);
            auto& calibration = field.band.calibration;
            if (config.mersi_equiv_mid_wn > 0) {
                calibration.kind = CalibrationKind::physical;
                const auto& main = file(SourceRole::data);
                double a, b;
                if (metadata_.satellite == "FY-3D") {
                    a = access_.attribute(main, "TBB_Trans_Coefficient_A").at(config.transid);
                    b = access_.attribute(main, "TBB_Trans_Coefficient_B").at(config.transid);
                } else {
                    const auto coefficients = access_.attribute(main, "TBB_Trans_Coefficient");
                    if (coefficients.size() % 2) throw std::invalid_argument("Odd TBB coefficient count");
                    a = coefficients.at(config.transid);
                    b = coefficients.at(config.transid + coefficients.size() / 2);
                }
                calibration.coefficients.insert(calibration.coefficients.end(), {config.mersi_equiv_mid_wn, a, b});
                calibration.provenance += "; main-file TBB coefficients and configured wavenumber";
                calibration.source = SourceRole::data;
                field.band.calibrated_unit = "K";
            } else {
                auto coefficient_path = "Calibration/" + get_viscal_name(metadata_.satellite);
                if (!Access::exists(file(SourceRole::data), coefficient_path)) continue;
                auto coefficients = file(SourceRole::data).openDataSet(coefficient_path);
                auto dims_cal = Access::dimensions(coefficients);
                if (dims_cal.size() != 2 || dims_cal[1] != 3 || config.transid >= dims_cal[0])
                    throw std::invalid_argument("Invalid MERSI VIS coefficient table");
                hsize_t origin[]{config.transid, 0}, count[]{1, 3};
                auto row = access_.read(coefficients, origin, count);
                calibration.kind = CalibrationKind::polynomial;
                calibration.coefficients.insert(calibration.coefficients.end(), row.begin(), row.end());
                calibration.provenance += "; " + coefficient_path;
                calibration.source = SourceRole::data;
                field.band.calibrated_unit = metadata_.satellite == "FY-3G" ? "1" : "%";
            }
            fields_.emplace(field.band.id, std::move(field));
        }
        if (!quarter && Access::exists(file(source), "Data/EV_1KM_LL")) {
            auto field = describe(low_light, source, "Data/EV_1KM_LL", grid, -1, 0);
            field.band.calibrated_unit = "radiance";
            fields_.emplace(low_light, std::move(field));
        }
    }

    void add_angle(int id, const std::string& path) {
        if (!Access::exists(file(SourceRole::geolocation), path)) return;
        auto field = describe(id, SourceRole::geolocation, path, "geo", -1, 0);
        field.band.calibrated_unit = "degree";
        fields_.emplace(id, std::move(field));
    }

    void add_mwri(const std::string& grid, const std::string& path, std::size_t channels,
                  int id_offset, bool detect_axis) {
        auto dataset = file(SourceRole::data).openDataSet(path);
        auto dims = Access::dimensions(dataset);
        if (dims.size() != 3) throw std::invalid_argument("MWRI data must have three axes");
        int axis = 2;
        if (detect_axis) {
            axis = -1;
            const auto shape = metadata_.grid(grid).shape;
            for (int candidate = 0; candidate < 3; ++candidate) {
                if (dims[candidate] != channels) continue;
                std::vector<hsize_t> spatial;
                for (int i = 0; i < 3; ++i) if (i != candidate) spatial.push_back(dims[i]);
                if (spatial[0] == shape.rows && spatial[1] == shape.cols) {
                    if (axis >= 0) throw std::invalid_argument("Ambiguous MWRI channel axis");
                    axis = candidate;
                }
            }
        }
        if (axis < 0 || dims[axis] != channels) throw std::invalid_argument("Invalid MWRI channel axis");
        for (std::size_t channel = 0; channel < channels; ++channel) {
            auto field = describe(static_cast<int>(channel) + id_offset, SourceRole::data, path, grid, axis, channel);
            field.band.calibrated_unit = "K";
            if (metadata_.satellite == "FY-3D" && field.band.fill_values.empty())
                field.band.fill_values.push_back(-32767);
            fields_.emplace(field.band.id, std::move(field));
        }
    }
};

}

HdfSummary read_hdf_summary(const std::filesystem::path& path) try {
    H5::H5File data(path.string(),H5F_ACC_RDONLY);
    const auto root = data.openGroup("/");
    const Access access{};
    HdfSummary result{access.text(root,"Satellite Name"),access.text(root,"Sensor Identification Code",false),
                      access.text(root,"File Alias Name",false)};
    if (result.sensor.empty() && Access::exists(data,"SLV/zFactorCorrectedESurface") &&
        Access::exists(data,"Geo_Fields/Latitude") && Access::exists(data,"Geo_Fields/Longitude")) result.sensor = "PMR";
    if (result.sensor.empty()) throw std::invalid_argument("Missing HDF attribute: Sensor Identification Code");
    return result;
} catch (const H5::Exception& error) {
    throw std::runtime_error("HDF5 metadata read: " + error.getDetailMsg());
}

std::string identify_hdf_adapter(const core::SourceFiles& sources) try {
    H5::H5File data(sources.data.string(),H5F_ACC_RDONLY);
    const auto root = data.openGroup("/");
    const Access access{};
    const auto satellite = access.text(root,"Satellite Name");
    const auto sensor = access.text(root,"Sensor Identification Code",false);
    if (satellite.rfind("FY-3",0) == 0) {
        if (sensor.find("MERSI") != std::string::npos) return "mersi";
        if (sensor.find("MWRI") != std::string::npos) return "mwri";
        if ((sensor.empty() || sensor.find("PMR") != std::string::npos) &&
            Access::exists(data,"SLV/zFactorCorrectedESurface")) {
            std::unique_ptr<H5::H5File> separate;
            if (!sources.geolocation.empty() && sources.geolocation != sources.data)
                separate = std::make_unique<H5::H5File>(sources.geolocation.string(),H5F_ACC_RDONLY);
            const auto& geo = separate ? *separate : data;
            if (Access::exists(geo,"Geo_Fields/Latitude") && Access::exists(geo,"Geo_Fields/Longitude")) return "pmr";
        }
    }
    if (satellite.rfind("FY-4",0) == 0) {
        if (sensor == "AGRI" || sensor == "GHI") return "fy4";
        if (sensor == "GIIRS") return "giirs";
    }
    throw std::invalid_argument("Unsupported HDF sensor or missing sensor datasets");
} catch (const H5::Exception& error) {
    throw std::runtime_error("HDF5 sensor identification: " + error.getDetailMsg());
}

void register_hdf_adapters(core::AdapterRegistry& registry, std::optional<core::ProfileReport> report) {
    for (const std::string kind : {"mersi", "mwri", "fy4", "pmr", "giirs"})
        registry.add(kind, [kind, report](const core::SourceFiles& sources) {
            try { return std::make_unique<HdfAdapter>(sources, kind, report); }
            catch (const H5::Exception& error) {
                throw std::runtime_error("HDF5 adapter open: " + error.getDetailMsg());
            }
        });
}

}
