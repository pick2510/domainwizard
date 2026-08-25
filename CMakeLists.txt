cmake_minimum_required(VERSION 3.21)
project(wrftools VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(GDAL 3.6 REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)

add_library(wrftools_core
    cpp/src/domain.cpp
    cpp/src/units.cpp
    cpp/src/colormaps.cpp
    cpp/src/raster_layer.cpp
    cpp/src/colorbar.cpp
    cpp/src/wps_namelist.cpp
    cpp/src/wrf_file.cpp
    cpp/src/wrf_series.cpp
)
target_include_directories(wrftools_core PUBLIC cpp/include)
target_link_libraries(wrftools_core PUBLIC GDAL::GDAL Qt6::Core Qt6::Gui)
target_compile_options(wrftools_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(wrftools_cpp
    cpp/src/main.cpp
    cpp/src/main_window.cpp
    cpp/src/view_form.cpp
    cpp/src/domain_form.cpp
    cpp/src/tile_map_widget.cpp
)
set_target_properties(wrftools_cpp PROPERTIES OUTPUT_NAME wrftools)
target_link_libraries(wrftools_cpp PRIVATE wrftools_core Qt6::Gui Qt6::Widgets Qt6::Network)
target_compile_options(wrftools_cpp PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_executable(wrftools_core_tests cpp/tests/core_tests.cpp)
    target_link_libraries(wrftools_core_tests PRIVATE wrftools_core Catch2::Catch2WithMain)
    add_test(NAME wrftools_core_tests COMMAND wrftools_core_tests)
    set_tests_properties(wrftools_core_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_executable(wrftools_widget_tests cpp/tests/widget_tests.cpp cpp/src/tile_map_widget.cpp)
    target_include_directories(wrftools_widget_tests PRIVATE cpp/include)
    target_link_libraries(wrftools_widget_tests PRIVATE wrftools_core Catch2::Catch2 Qt6::Gui Qt6::Widgets Qt6::Network)
    add_test(NAME wrftools_widget_tests COMMAND wrftools_widget_tests)
    set_tests_properties(wrftools_widget_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CACHE_HOME=/tmp/wrftools-cpp-test-cache")
endif()
