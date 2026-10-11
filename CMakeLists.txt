cmake_minimum_required(VERSION 4.0)
project(pg_EV_image)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ZLIB REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(OpenCV REQUIRED)
find_package(TBB REQUIRED)
find_package(CLI11 REQUIRED)

add_library(fy_pipeline STATIC
        src/mersi.cpp
        src/pipeline.cpp
        include/pipeline.hpp
        include/raster.hpp
        include/legacy_bridge.hpp
)

add_executable(pg_EV_image
        src/app.cpp
        src/legacy.cpp
        include/legacy/config.h
        include/legacy/color.h
        include/legacy/phys.h
        include/legacy/rend.h
        include/legacy/read.h
        include/legacy/modes.h
        include/legacy/context.h
        include/legacy/modes_pmr_mwri.h
        include/legacy/modes_mersi.h
        include/legacy/modes_fy4.h
        include/legacy/modes_giirs.h
)

target_link_libraries(fy_pipeline PUBLIC
        ${OpenCV_LIBS}
        ZLIB::ZLIB
        ${HDF5_LIBRARIES}
        ${HDF5_CXX_LIBRARIES}
        TBB::tbb
        CLI11::CLI11
)

target_include_directories(fy_pipeline PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/legacy
        ${OpenCV_INCLUDE_DIRS}
        ${HDF5_INCLUDE_DIRS}
)

target_link_libraries(pg_EV_image PRIVATE fy_pipeline)

include(CTest)
if(BUILD_TESTING)
    add_executable(projection_test tests/projection_test.cpp)
    target_link_libraries(projection_test PRIVATE fy_pipeline)
    add_test(NAME projection_test COMMAND projection_test)
    add_executable(rtec_color_test tests/rtec_color_test.cpp)
    target_link_libraries(rtec_color_test PRIVATE fy_pipeline)
    add_test(NAME rtec_color_test COMMAND rtec_color_test)
    add_executable(legacy_grid_test tests/legacy_grid_test.cpp)
    target_link_libraries(legacy_grid_test PRIVATE fy_pipeline)
    add_test(NAME legacy_grid_test COMMAND legacy_grid_test)
endif()
