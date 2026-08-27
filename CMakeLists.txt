cmake_minimum_required(VERSION 3.21)
project(wrftools VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(GDAL 3.6 REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)

# NetCDF-C, linked directly (not through GDAL's netCDF driver) for
# NetcdfFile (netcdf_file.hpp/.cpp) - the w2w LCZ port's read/write
# foundation (see PORT_W2W.MD Stage 0). GDAL's raster-dataset abstraction
# can read WRF NetCDF but cannot round-trip its non-raster dimensions
# (land_cat, month, num_urb_params) losslessly, which writing a modified
# geo_em file back out requires.
find_package(PkgConfig REQUIRED)
pkg_check_modules(NETCDF REQUIRED IMPORTED_TARGET netcdf)

# GeoTIFF <-> WPS geogrid conversion, ported from
# https://github.com/jbeezley/convert_geotiff (public domain) - its own
# small, GDAL-free library, kept in its own convert_geotiff/ namespace and
# directories since it's a vendored subsystem, not part of wrftools_core's
# own geospatial pipeline. Only its FLTK GUI was replaced (see
# GeotiffConvertForm); the conversion logic itself (src/convert_geotiff/)
# is unmodified from upstream.
find_library(TIFF_LIBRARY NAMES tiff)
find_path(TIFF_INCLUDE_DIR NAMES tiffio.h)
if(NOT TIFF_LIBRARY OR NOT TIFF_INCLUDE_DIR)
    message(FATAL_ERROR "Cannot find TIFF library/headers (libtiff-dev on Debian/Ubuntu, libtiff-devel on Fedora).")
endif()
find_library(GEOTIFF_LIBRARY NAMES geotiff)
find_path(GEOTIFF_INCLUDE_DIR NAMES geotiffio.h PATH_SUFFIXES geotiff libgeotiff)
if(NOT GEOTIFF_LIBRARY OR NOT GEOTIFF_INCLUDE_DIR)
    message(FATAL_ERROR "Cannot find GEOTIFF library/headers (libgeotiff-dev on Debian/Ubuntu, libgeotiff-devel on Fedora).")
endif()

# The index-file/tile-data reader (parsing only - directory listing and
# text/binary file I/O, no TIFF/GEOTIFF dependency) is split out from the
# rest of convert_geotiff so wrftools_core can reuse it (for
# WpsBinarySource, a ViewForm-visible layer backed by a raw WPS_GEOG
# dataset) without pulling libtiff/libgeotiff into wrftools_core - only the
# GUI's Convert tab needs those, for actual GeoTIFF conversion.
add_library(convert_geotiff_reader
    src/convert_geotiff/geogrid_index.cpp
    src/convert_geotiff/geogrid_reader.cpp
)
target_include_directories(convert_geotiff_reader PUBLIC include)

add_library(convert_geotiff_lib
    src/convert_geotiff/geotiff_reader.cpp
    src/convert_geotiff/geogrid_tile_writer.cpp
    src/convert_geotiff/convert.cpp
    src/convert_geotiff/geotiff_writer.cpp
    src/convert_geotiff/convert_back.cpp
)
target_include_directories(convert_geotiff_lib PUBLIC include ${TIFF_INCLUDE_DIR} ${GEOTIFF_INCLUDE_DIR})
target_link_libraries(convert_geotiff_lib PUBLIC convert_geotiff_reader ${TIFF_LIBRARY} ${GEOTIFF_LIBRARY})

add_library(wrftools_core
    src/crs.cpp
    src/warp.cpp
    src/domain.cpp
    src/units.cpp
    src/colormaps.cpp
    src/raster_layer.cpp
    src/layer_renderer.cpp
    src/colorbar.cpp
    src/wps_namelist.cpp
    src/wrf_file.cpp
    src/wrf_series.cpp
    src/wrf_source.cpp
    src/wps_binary_source.cpp
    src/netcdf_file.cpp
)
target_include_directories(wrftools_core PUBLIC include)
target_link_libraries(wrftools_core PUBLIC GDAL::GDAL Qt6::Core Qt6::Gui convert_geotiff_reader PkgConfig::NETCDF)
target_compile_options(wrftools_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(wrftools_ui
    src/theme.cpp
    src/main_window.cpp
    src/view_form.cpp
    src/domain_form.cpp
    src/domain_overlay.cpp
    src/tile_map_widget.cpp
    src/geotiff_convert_form.cpp
)
target_include_directories(wrftools_ui PUBLIC include)
target_link_libraries(wrftools_ui PUBLIC wrftools_core convert_geotiff_lib Qt6::Gui Qt6::Widgets Qt6::Network)
target_compile_options(wrftools_ui PRIVATE -Wall -Wextra -Wpedantic)

add_executable(wrftools_cpp src/main.cpp)
set_target_properties(wrftools_cpp PROPERTIES OUTPUT_NAME wrftools)
target_link_libraries(wrftools_cpp PRIVATE wrftools_ui Qt6::Gui)
target_compile_options(wrftools_cpp PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS wrftools_cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(APPLE)
    set_target_properties(wrftools_cpp PROPERTIES MACOSX_BUNDLE TRUE)
elseif(WIN32)
    # Without this, add_executable() links wrftools.exe as a console-
    # subsystem app, so Windows always opens a console window behind the
    # Qt GUI on launch (Linux/macOS have no such distinction).
    set_target_properties(wrftools_cpp PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    include(Catch)
    add_executable(wrftools_core_tests tests/core_tests.cpp)
    # Catch2::Catch2 (not ...WithMain) - core_tests.cpp provides its own
    # main() so it can quick_exit() after Catch::Session().run(), see there.
    target_link_libraries(wrftools_core_tests PRIVATE wrftools_core Catch2::Catch2)
    catch_discover_tests(wrftools_core_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # catch_discover_tests runs the test binary at build time (not just at
    # ctest time) to enumerate cases, and this installed Catch2 has no
    # DISCOVERY_MODE PRE_TEST to defer that. A Qt GUI binary needs the
    # offscreen platform plugin for that build-time run too, which isn't
    # guaranteed to be in the builder's environment (CI doesn't set it), so
    # the two Qt-linked test binaries stay single-entry add_test like before;
    # only the headless core suite gets per-case discovery.
    add_executable(wrftools_widget_tests tests/widget_tests.cpp)
    target_link_libraries(wrftools_widget_tests PRIVATE wrftools_ui Catch2::Catch2)
    add_test(NAME wrftools_widget_tests COMMAND wrftools_widget_tests)
    set_tests_properties(wrftools_widget_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CACHE_HOME=/tmp/wrftools-cpp-test-cache")

    add_executable(wrftools_ui_tests tests/ui_tests.cpp)
    target_link_libraries(wrftools_ui_tests PRIVATE wrftools_ui Catch2::Catch2)
    add_test(NAME wrftools_ui_tests COMMAND wrftools_ui_tests)
    set_tests_properties(wrftools_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CACHE_HOME=/tmp/wrftools-cpp-test-cache" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
