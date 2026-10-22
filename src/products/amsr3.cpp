/// SPDX-License-Identifier: MIT

#include "common.hpp"

namespace fy::products {

ProductDefinition amsr3_product(const core::SensorMetadata& metadata,std::span<const core::BandDescriptor> bands,
                               const ProductRequest& request) {
    using namespace detail;
    if (request.scan != "A" && request.scan != "B") throw std::invalid_argument("AMSR3 product requires scan A or B");
    const auto& mode = request.mode;
    const int v = request.scan == "A" ? 14 : 16,h = v+1;
    std::vector<int> ids;
    std::string description;
    if (mode == "W") ids = {request.band};
    else if (mode == "W84") { ids = {v,h}; description = "W84: 89GHz PCT / V / H"; }
    else if (mode == "TCX") { ids = {v,h,12}; description = "TCX: PCT89 / Pol / Scat"; }
    else if (mode == "TCS") { ids = {v,12,10,8}; description = "TCS: 89-36 / 36-18 / 23-18"; }
    else if (mode == "TCR") { ids = {v,h,10,12}; description = "TCR: PCT89 / 23V / 36V"; }
    else throw std::invalid_argument("Unsupported AMSR3 mode: "+mode+"; available: W W84 TCX TCS TCR");
    ProductDefinition result;
    result.plan.mode = mode;
    result.plan.clip_to_region = true;
    result.plan.calculation_grid = band_at(bands,ids.front()).grid;
    for (int id : ids) {
        const auto& band = band_at(bands,id);
        if (band.calibrated_unit != "K") throw std::invalid_argument("AMSR3 requires kelvin inputs");
        result.plan.inputs.push_back({.band = id,.target_grid = result.plan.calculation_grid,.geographic_alignment = true});
    }
    result.plan.frames.push_back({.name = mode,.units = std::vector<std::string>(ids.size(),"K")});
    if (mode == "W") {
        description = band_at(bands,ids.front()).name+" #"+std::to_string(request.band)+" [K]";
        StatisticsSpec spec;
        spec.operation = StatisticsOperation::minmax; spec.field = "brightness_temperature"; spec.before_crop = false;
        result.plan.statistics.push_back(spec);
    } else description += " [89"+request.scan+"]";
    result.plan.validate(metadata,bands);
    result.prepare = [metadata,mode,description,count = ids.size()](std::vector<ScalarPlane>&& inputs,
                     std::shared_ptr<const NativeGeometry> geometry,ExecutionContext&) {
        if (!geometry) throw std::invalid_argument("Missing AMSR3 geometry");
        validate_planes(inputs,count,geometry_shape(*geometry));
        PreparedProduct product;
        product.frame.name = mode; product.frame.geometry = std::move(geometry);
        product.labels = labels(metadata,description,false);
        if (mode == "W") {
            const auto& swath = std::get<SwathGeometry>(*product.frame.geometry);
            for (std::size_t i = 0; i < inputs.front().valid.values().size(); ++i)
                if (!swath.valid.values()[i]) { inputs.front().valid.values()[i] = 0; inputs.front().values.values()[i] = NAN; }
            product.display = scalar_display("",palette::color_plasma,extrema(inputs.front(),-INFINITY,INFINITY));
        } else product.display.mapper = [mode](std::span<const float> v,const DisplayScale&) {
            const float pct = float(1.818*v[0]-.818*v[1]);
            if (mode == "W84") return rgb((300-pct)*1.5f,(300-v[0])*1.5f,(300-v[1])*1.5f,255);
            if (mode == "TCX") return rgb((300-pct)*1.5f,(v[0]-v[1])*5,float((v[0]-v[2]+80)*1.8),255);
            if (mode == "TCS") return rgb((v[0]-v[1]+50)*2.5f,(v[1]-v[3]+17)*5,(v[2]-v[3]+17)*5,255);
            return rgb((300-pct)*1.5f,(300-v[2])*1.5f,(300-v[3])*1.5f,255);
        };
        product.frame.planes = std::move(inputs); product.frame.validate();
        return product;
    };
    return result;
}

}
