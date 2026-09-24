cmake_minimum_required(VERSION 4.0)
project(pg_EV_image)

set(CMAKE_CXX_STANDARD 23)
#set(CMAKE_CXX_FLAGS -fsanitize=undefined,address)

find_package(ZLIB REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(OpenCV REQUIRED)
find_package(TBB REQUIRED)
find_package(CLI11 REQUIRED)

add_executable(pg_EV_image
        main.cpp
        config.h
        color.h
        phys.h
        rend.h
        read.h
        modes.h
        context.h
        modes_pmr_mwri.h
        modes_mersi.h
        modes_fy4.h
        modes_giirs.h
)

target_link_libraries(pg_EV_image
        ${OpenCV_LIBS}
        ZLIB::ZLIB
        ${HDF5_LIBRARIES}
        ${HDF5_CXX_LIBRARIES}
        TBB::tbb
        CLI11::CLI11
)

target_include_directories(pg_EV_image PUBLIC
        ${OpenCV_INCLUDE_DIRS}
        ${HDF5_INCLUDE_DIRS}
)
