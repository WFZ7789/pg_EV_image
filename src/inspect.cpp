/// SPDX-License-Identifier: MIT

#include "pipeline.hpp"
#include "sensors/hdf.hpp"
#include "sensors/input.hpp"

#include <iostream>

namespace fy {

void inspect(const std::filesystem::path& original) {
    const core::ScratchDirectory scratch;
    const auto data = sensors::resolve_data_input(original,scratch);
    const auto summary = core::IoChannel::shared()->run([&] { return sensors::read_hdf_summary(data); });
    std::cout << "Satellite: " << summary.satellite << '\n'
              << "Sensor: " << summary.sensor << '\n'
              << "Alias: " << summary.alias << '\n'
              << "Data: " << original << '\n';
    if (summary.sensor == "METimage") {
        core::AdapterRegistry registry;
        sensors::register_hdf_adapters(registry);
        auto session = registry.open("metimage",{data,{},{}});
        const auto metadata = session.metadata();
        std::cout << "Observation: " << metadata.observation_begin << " / " << metadata.observation_end << '\n'
                  << "Modes: R RMUL E RT RY RF EC ECT TSG\n";
        for (const auto& band : session.bands())
            if (band.id >= 0) std::cout << "Band " << band.id << ": " << band.name << " [" << band.calibrated_unit << "]\n";
    }
    else if (summary.sensor == "AMSR3") {
        core::AdapterRegistry registry;
        sensors::register_hdf_adapters(registry);
        auto session = registry.open("amsr3",{data,{},{}});
        const auto metadata = session.metadata();
        std::cout << "Observation: " << metadata.observation_begin << " / " << metadata.observation_end << '\n'
                  << "Modes: W W84 TCX TCS TCR\nScans: A B AB (composites; default A)\n";
        for (const auto& band : session.bands()) std::cout << "Band " << band.id << ": " << band.name << " [" << band.grid << "]\n";
    }
    else if (summary.sensor.find("MERSI") != std::string::npos)
        std::cout << "Modes: E R LL RT RY RF RMUL EC ECT TSG (实际支持情况依卫星和分辨率而定)\n";
    else if (summary.sensor.find("MWRI") != std::string::npos)
        std::cout << "Modes: S W W84 TCX TCN TCN2 TCS TCR (依卫星和通道而定)\n";
    else if (summary.sensor.find("PMR") != std::string::npos)
        std::cout << "Modes: PMR\n";
    else if (summary.sensor == "GIIRS")
        std::cout << "Modes: R LW MW IRall\n";
    else if (summary.satellite.rfind("FY-4",0) == 0)
        std::cout << "Modes: E R Re Rr EC RF RY RX RTpp RTEC RTEC16 RTG AGHI_ALD RT16 RT+EC\n";
}

}
