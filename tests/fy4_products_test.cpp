/// SPDX-License-Identifier: MIT

#include "products/execute.hpp"
#include "products/fy4.hpp"
#include "sensors/hdf.hpp"
#include "product_reference.hpp"

#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cmath>
#include "fy4_fixture.hpp"

#include <iostream>
#include <source_location>

namespace {

namespace core = fy::core;

void require(bool condition,std::source_location location = std::source_location::current()) {
    if (!condition) throw std::runtime_error("FY4 product check failed at line " + std::to_string(location.line()));
}


int compare(const fy::products::PreparedProduct& product,const cv::Mat& reference,bool rounding,const std::string& mode) {
    const auto shape = product.frame.planes.front().values.shape();
    require(shape == core::Shape{std::size_t(reference.rows),std::size_t(reference.cols)});
    const int maximum_value = reference.depth() == CV_16U ? 65535 : 255;
    std::vector<float> values(product.frame.planes.size());
    std::size_t common = 0;
    int maximum = 0;
    for (std::size_t row = 0; row < shape.rows; ++row)
        for (std::size_t col = 0; col < shape.cols; ++col) {
            const auto pixel = row * shape.cols + col;
            bool valid = true;
            for (std::size_t channel = 0; channel < values.size(); ++channel) {
                valid = valid && product.frame.planes[channel].valid.values()[pixel];
                values[channel] = product.frame.planes[channel].values.values()[pixel];
            }
            if (!valid) continue;
            ++common;
            const auto color = product.display.mapper(values,product.display.scale);
            const std::array channels{color.blue,color.green,color.red};
            for (int channel = 0; channel < 3; ++channel) {
                require(std::isfinite(channels[channel]));
                const float value = float(channels[channel]) * maximum_value;
                const auto quantized = std::clamp<int>(rounding ? int(std::llround(value)) : int(value),0,maximum_value);
                const int expected = reference.depth() == CV_16U ? reference.at<cv::Vec<std::uint16_t,3>>(int(row),int(col))[channel] :
                                                                   reference.at<cv::Vec3b>(int(row),int(col))[channel];
                maximum = std::max(maximum,std::abs(quantized - expected));
            }
        }
    std::cout << mode << " common=" << common << " max quantized difference=" << maximum << '\n';
    require(common > 0 && maximum <= 1);
    return maximum;
}

void check(core::SourceFiles files,const std::string& mode,int band,core::ExecutionContext& context) {
    core::AdapterRegistry registry;
    fy::sensors::register_hdf_adapters(registry,context.profile());
    auto session = registry.open("fy4",files);
    fy::products::ProductRequest request;
    request.mode = mode; request.band = band;
    const auto program = fy::products::fy4_product(session.metadata(),session.bands(),request);
    auto product = fy::products::load_product(session,program,context);
    const auto frozen = product_reference::next("fy4");
    cv::Mat expected(int(frozen.rows),int(frozen.cols),frozen.depth == 16 ? CV_16UC3 : CV_8UC3);
    for (std::size_t row = 0; row < frozen.rows; ++row)
        for (std::size_t col = 0; col < frozen.cols; ++col)
            for (int channel = 0; channel < 3; ++channel) {
                const auto value = frozen.pixels[row * frozen.cols + col][channel];
                if (frozen.depth == 16) expected.at<cv::Vec<std::uint16_t,3>>(int(row),int(col))[channel] = value;
                else expected.at<cv::Vec3b>(int(row),int(col))[channel] = static_cast<std::uint8_t>(value);
            }
    compare(product.product,expected,program.plan.mode == "RTEC" || program.plan.mode == "RTEC16",mode);
    require(product.product.labels == frozen.labels);
}

}

int main(int argc,char** argv) {
    core::ExecutionContext context({.threads = 4});
    if (argc == 4 && std::string(argv[1]) == "--compare-images") {
        const auto first = cv::imread(argv[2],cv::IMREAD_UNCHANGED),second = cv::imread(argv[3],cv::IMREAD_UNCHANGED);
        require(!first.empty() && first.type() == CV_16UC3 && first.type() == second.type() && first.size() == second.size());
        require(cv::norm(first,second,cv::NORM_INF) == 0);
        double high = 0;
        cv::minMaxLoc(first.reshape(1),nullptr,&high);
        require(high > 255);
        std::cout << "16-bit decoded images equal: " << first.cols << 'x' << first.rows << '\n';
        return 0;
    }
    require(argc == 1);
    for (const std::string satellite : {"FY-4A","FY-4B"}) {
        const auto primary = context.scratch().path() / "primary.HDF", auxiliary = context.scratch().path() / "auxiliary.HDF";
        context.io().run([&] { fy4_fixture::write(primary,satellite,"AGRI",32,48,false); fy4_fixture::write(auxiliary,satellite,"AGRI",16,24,false); });
        for (const std::string mode : {"E","R","Re","Rr","EC","RF","RY","RX","RTpp","RTEC","RTEC16","RT16","RT+EC","AGHI_ALD"})
            check({primary,primary,{}},mode,mode == "E" ? 13 : 2,context);
        for (const std::string mode : {"RF","RY","RTEC","RTEC16"}) check({primary,primary,auxiliary},mode,2,context);
    }
    {
        const auto primary = context.scratch().path() / "ghi.HDF", auxiliary = context.scratch().path() / "ghi-auxiliary.HDF";
        context.io().run([&] { fy4_fixture::write(primary,"FY-4B","GHI",32,48,false); fy4_fixture::write(auxiliary,"FY-4B","GHI",64,96,false); });
        for (const std::string mode : {"E","R","Re","Rr","RF","RY","RX","RTpp","RTG","AGHI_ALD"})
            check({primary,primary,{}},mode,mode == "E" ? 7 : 4,context);
        check({primary,primary,auxiliary},"RTG",4,context);
    }
    const auto report = context.profile().snapshot();
    for (const auto& buffer : *report.buffers) require(buffer.current_bytes == 0);
}
