cmake_minimum_required(VERSION 4.0)
project(pg_EV_image)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(TBB REQUIRED)
find_package(Threads REQUIRED)
option(FY_BUILD_APP "Build the satellite application and native output layer" ON)

add_library(fy_core STATIC
        src/core/model.cpp
        src/core/navigation.cpp
        src/core/adapter.cpp
        src/core/profile.cpp
        src/core/execution.cpp
        src/core/calibration.cpp
        src/core/resample.cpp
        src/core/geographic_resample.cpp
        src/core/stream.cpp
        src/core/render.cpp
        src/core/image_render.cpp
)
target_include_directories(fy_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fy_core PRIVATE TBB::tbb Threads::Threads)
target_compile_features(fy_core PUBLIC cxx_std_23)

add_library(fy_products STATIC src/products/swath.cpp src/products/amsr3.cpp src/products/rayleigh.cpp src/products/fy4.cpp src/products/fy4_day.cpp src/products/pmr.cpp src/products/giirs.cpp src/products/execute.cpp src/products/cache.cpp src/products/statistics.cpp src/products/registry.cpp)
target_link_libraries(fy_products PUBLIC fy_core)

if(FY_BUILD_APP)
    find_package(HDF5 REQUIRED COMPONENTS C CXX)
    find_package(OpenCV REQUIRED)
    find_package(CLI11 REQUIRED)
    find_package(ZLIB REQUIRED)

    add_library(fy_sensors STATIC src/sensors/hdf.cpp src/sensors/input.cpp)
    target_link_libraries(fy_sensors PUBLIC fy_core PRIVATE ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES} ZLIB::ZLIB)
    target_include_directories(fy_sensors PRIVATE ${HDF5_INCLUDE_DIRS})

    add_library(fy_render STATIC src/native_swath.cpp src/render_product.cpp src/inspect.cpp)
    target_link_libraries(fy_render PUBLIC fy_products fy_sensors PRIVATE ${OpenCV_LIBS})
    target_include_directories(fy_render PRIVATE ${OpenCV_INCLUDE_DIRS})

    add_executable(pg_EV_image src/app.cpp)
    target_link_libraries(pg_EV_image PRIVATE fy_render CLI11::CLI11 ${OpenCV_LIBS})
    target_include_directories(pg_EV_image PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()

include(CTest)
if(BUILD_TESTING)
    foreach(core_test IN ITEMS core_model_test core_adapter_test core_execution_test core_resample_test geographic_resample_test core_calibration_test core_stream_test core_render_test core_image_render_test)
        add_executable(${core_test} tests/${core_test}.cpp)
        target_link_libraries(${core_test} PRIVATE fy_core)
        add_test(NAME ${core_test} COMMAND ${core_test})
        set_tests_properties(${core_test} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(swath_cache_test tests/swath_cache_test.cpp)
    target_link_libraries(swath_cache_test PRIVATE fy_products)
    add_test(NAME swath_cache_test COMMAND swath_cache_test)
    set_tests_properties(swath_cache_test PROPERTIES TIMEOUT 30)
    add_executable(rayleigh_test tests/rayleigh_test.cpp)
    target_link_libraries(rayleigh_test PRIVATE fy_products)
    add_test(NAME rayleigh_test COMMAND rayleigh_test)
    add_executable(scan_shadow_test tests/scan_shadow_test.cpp)
    target_link_libraries(scan_shadow_test PRIVATE fy_products)
    add_test(NAME scan_shadow_test COMMAND scan_shadow_test)
    if(FY_BUILD_APP)
        add_executable(swath_products_test tests/swath_products_test.cpp)
        target_link_libraries(swath_products_test PRIVATE fy_products fy_sensors ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
        target_include_directories(swath_products_test PRIVATE ${HDF5_INCLUDE_DIRS})
        add_test(NAME swath_products_test COMMAND swath_products_test)
        add_executable(hdf_adapter_test tests/hdf_adapter_test.cpp)
        target_link_libraries(hdf_adapter_test PRIVATE fy_sensors ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
        target_include_directories(hdf_adapter_test PRIVATE ${HDF5_INCLUDE_DIRS})
        add_test(NAME hdf_adapter_test COMMAND hdf_adapter_test)
        add_executable(fy4_adapter_test tests/fy4_adapter_test.cpp)
        target_link_libraries(fy4_adapter_test PRIVATE fy_sensors ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
        target_include_directories(fy4_adapter_test PRIVATE ${HDF5_INCLUDE_DIRS})
        add_test(NAME fy4_adapter_test COMMAND fy4_adapter_test)
        add_executable(fy4_products_test tests/fy4_products_test.cpp)
        target_link_libraries(fy4_products_test PRIVATE fy_products fy_sensors ${OpenCV_LIBS} ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
        target_include_directories(fy4_products_test PRIVATE ${OpenCV_INCLUDE_DIRS} ${HDF5_INCLUDE_DIRS})
        add_test(NAME fy4_products_test COMMAND fy4_products_test)
        add_executable(pmr_products_test tests/pmr_products_test.cpp)
        target_link_libraries(pmr_products_test PRIVATE fy_products fy_sensors ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
        target_include_directories(pmr_products_test PRIVATE ${HDF5_INCLUDE_DIRS})
        add_test(NAME pmr_products_test COMMAND pmr_products_test)
        add_executable(giirs_products_test tests/giirs_products_test.cpp)
        target_link_libraries(giirs_products_test PRIVATE fy_products fy_sensors ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
        target_include_directories(giirs_products_test PRIVATE ${HDF5_INCLUDE_DIRS})
        add_test(NAME giirs_products_test COMMAND giirs_products_test)
        add_executable(amsr3_test tests/amsr3_test.cpp)
        target_link_libraries(amsr3_test PRIVATE fy_render ${OpenCV_LIBS} ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
        target_include_directories(amsr3_test PRIVATE ${OpenCV_INCLUDE_DIRS} ${HDF5_INCLUDE_DIRS})
        add_test(NAME amsr3_test COMMAND amsr3_test)
        add_executable(metimage_test tests/metimage_test.cpp)
        target_link_libraries(metimage_test PRIVATE fy_render ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
        target_include_directories(metimage_test PRIVATE ${HDF5_INCLUDE_DIRS})
        add_test(NAME metimage_test COMMAND metimage_test)
        add_executable(new_sensor_test tests/new_sensor_test.cpp)
        target_link_libraries(new_sensor_test PRIVATE fy_render ${OpenCV_LIBS})
        target_include_directories(new_sensor_test PRIVATE ${OpenCV_INCLUDE_DIRS})
        add_test(NAME new_sensor_test COMMAND new_sensor_test)
        add_executable(sensor_reference tools/sensor_reference.cpp)
        target_link_libraries(sensor_reference PRIVATE fy_sensors)
        add_executable(check_palette_reference tools/check_palette_reference.cpp)
        target_link_libraries(check_palette_reference PRIVATE fy_products)
        find_package(Python3 COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_executable(cli_fixture tests/cli_fixture.cpp)
            target_include_directories(cli_fixture PRIVATE ${HDF5_INCLUDE_DIRS})
            target_link_libraries(cli_fixture PRIVATE ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES})
            add_test(NAME amsr3_cli_test COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/amsr3_cli_test.py
                    --app $<TARGET_FILE:pg_EV_image> --fixture $<TARGET_FILE:amsr3_test>)
            set_tests_properties(amsr3_cli_test PROPERTIES TIMEOUT 120)
            add_test(NAME metimage_cli_test COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/metimage_cli_test.py
                    --app $<TARGET_FILE:pg_EV_image> --fixture $<TARGET_FILE:metimage_test>)
            set_tests_properties(metimage_cli_test PROPERTIES TIMEOUT 120)
            add_test(NAME cli_execution_test COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_execution_test.py
                    --app $<TARGET_FILE:pg_EV_image> --fixture $<TARGET_FILE:cli_fixture>)
            set_tests_properties(cli_execution_test PROPERTIES TIMEOUT 120)
            add_test(NAME fy4_cli_test COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fy4_cli_test.py
                    --app $<TARGET_FILE:pg_EV_image> --fixture $<TARGET_FILE:cli_fixture>)
            set_tests_properties(fy4_cli_test PROPERTIES TIMEOUT 120)
            add_test(NAME pmr_cli_test COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/pmr_cli_test.py
                    --app $<TARGET_FILE:pg_EV_image> --fixture $<TARGET_FILE:cli_fixture>)
            set_tests_properties(pmr_cli_test PROPERTIES TIMEOUT 120)
            add_test(NAME giirs_cli_test COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/giirs_cli_test.py
                    --app $<TARGET_FILE:pg_EV_image> --fixture $<TARGET_FILE:cli_fixture>)
            set_tests_properties(giirs_cli_test PROPERTIES TIMEOUT 120)
            add_test(NAME native_layout_test COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_layout_test.py
                    --app $<TARGET_FILE:pg_EV_image> --fixture $<TARGET_FILE:cli_fixture>)
            set_tests_properties(native_layout_test PROPERTIES TIMEOUT 120)
            add_test(NAME reference_integrity_test COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/reference_integrity_test.py)
        endif()
        add_executable(projection_test tests/projection_test.cpp)
        target_link_libraries(projection_test PRIVATE fy_core)
        add_test(NAME projection_test COMMAND projection_test)
        add_executable(rtec_color_test tests/rtec_color_test.cpp)
        target_link_libraries(rtec_color_test PRIVATE fy_products)
        add_test(NAME rtec_color_test COMMAND rtec_color_test)
    endif()
endif()
