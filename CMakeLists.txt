cmake_minimum_required(VERSION 3.21)
project(wrftools VERSION 0.1.0 LANGUAGES CXX)

if(WIN32)
    # Needed to compile resources/wrftools.rc (the .exe's icon) via
    # windres/rc.exe.
    enable_language(RC)
endif()

# On Linux, when Homebrew/Linuxbrew is on this machine, prefer
# system-installed libraries over what it exposes earlier on PATH. CMake's
# find_package() Config-mode search checks PATH-derived prefixes (e.g.
# Homebrew's, from its bin/ being on PATH) before the default system
# prefixes - so without this, Qt6 below can resolve from Homebrew while
# GDAL resolves from the system package, baking a Homebrew RUNPATH into
# every binary. At runtime the dynamic loader then prefers that RUNPATH
# over the system default path, so even a library correctly *linked*
# against the system copy (netCDF, via the pkg-config guard further down)
# actually *loads* Homebrew's copy instead - and if that copy's HDF5
# differs from the one system GDAL links, both HDF5 builds end up loaded
# in one process and corrupt each other's internal state at runtime (a
# misleading "NetCDF: HDF error" on perfectly valid files, not a
# build-time error).
#
# Gated on HOMEBREW_PREFIX (set by Homebrew/Linuxbrew's shellenv) rather
# than applied unconditionally: prepending /usr unconditionally would also
# jump ahead of a user's own -DCMAKE_PREFIX_PATH (e.g. a conda/mamba
# environment, or a deliberately custom-built stack), silently overriding
# their choice instead of just correcting for Homebrew's PATH-hint quirk.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DEFINED ENV{HOMEBREW_PREFIX})
    list(PREPEND CMAKE_PREFIX_PATH /usr)
endif()

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 3.9, not 3.7: OGRSpatialReference::exportToCF1() (reproject.cpp's CF
# grid-mapping construction) landed in GDAL 3.9.0 - Ubuntu 24.04's own
# packaged libgdal-dev is only 3.8.4, which compiles but fails with "has no
# member named 'exportToCF1'"; see cpp.yml's Linux dependency step, which
# pulls a newer GDAL from the ubuntugis-unstable PPA specifically for this.
find_package(GDAL 3.9 REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network)

# NetCDF-C, linked directly (not through GDAL's netCDF driver) for
# NetcdfFile (netcdf_file.hpp/.cpp) - the w2w LCZ port's read/write
# foundation (see PORT_W2W.MD Stage 0). GDAL's raster-dataset abstraction
# can read WRF NetCDF but cannot round-trip its non-raster dimensions
# (land_cat, month, num_urb_params) losslessly, which writing a modified
# geo_em file back out requires.
#
# On Linux, when Homebrew/Linuxbrew is on this machine, its own
# pkg-config landing earlier on PATH than the system one can make
# pkg_check_modules() below resolve netCDF against a different HDF5 build
# than the one find_package(GDAL) above already resolved system GDAL
# against - even with the CMAKE_PREFIX_PATH guard above, since pkg-config
# has its own separate, PATH-driven executable search. Both HDF5 builds
# then load into the same process and corrupt each other's internal state
# at runtime - producing an intermittent, misleading "NetCDF: HDF error"
# on perfectly valid files, not a build-time error, which makes it easy to
# mistake for corrupt fixtures or a genuine code bug. Prefer the system
# pkg-config, which searches system paths first, when one exists.
#
# Gated on HOMEBREW_PREFIX rather than applied unconditionally, matching
# the CMAKE_PREFIX_PATH guard above - forcing the system pkg-config binary
# has little downside on its own (it still honors PKG_CONFIG_PATH, e.g.
# from an activated conda environment), but there is no reason to override
# the PATH-found one on a machine where Homebrew was never the problem.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DEFINED ENV{HOMEBREW_PREFIX})
    find_program(PKG_CONFIG_EXECUTABLE NAMES pkg-config PATHS /usr/bin /bin NO_DEFAULT_PATH)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(NETCDF REQUIRED IMPORTED_TARGET netcdf)

# Belt-and-suspenders: if GDAL and netCDF still resolved from different
# install prefixes (e.g. no system pkg-config exists, or some other
# toolchain mixing this doesn't account for), warn loudly rather than let
# the same class of bug resurface as a baffling test-time HDF5 crash.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND GDAL_LIBRARY)
    get_filename_component(_gdal_prefix "${GDAL_LIBRARY}" DIRECTORY)
    get_filename_component(_gdal_prefix "${_gdal_prefix}" DIRECTORY)
    foreach(_netcdf_libdir ${NETCDF_LIBRARY_DIRS})
        get_filename_component(_netcdf_prefix "${_netcdf_libdir}" DIRECTORY)
        if(NOT _netcdf_prefix STREQUAL _gdal_prefix)
            message(WARNING
                "GDAL is resolving from '${_gdal_prefix}' but netCDF from "
                "'${_netcdf_prefix}'. If they end up linking different HDF5 builds, "
                "mixing both in one process can corrupt HDF5's internal state at "
                "runtime (misleading 'NetCDF: HDF error' on otherwise-valid files). "
                "Point PKG_CONFIG_PATH/CMAKE_PREFIX_PATH at one consistent set of "
                "libraries, e.g. -DCMAKE_PREFIX_PATH=/usr on Fedora.")
        endif()
    endforeach()
endif()

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
    src/lcz.cpp
    src/reproject.cpp
    src/derived_variable.cpp
    src/stats.cpp
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
    src/lcz_form.cpp
    src/reproject_form.cpp
    src/chart_widget.cpp
    src/point_inspector_dialog.cpp
    # TimeSeriesDialog/RasterStatsDialog declare Q_OBJECT (needed for
    # qobject_cast in tests) but live in include/wrftools/, not next to
    # their .cpp in src/ - AUTOMOC's header/source pairing only looks in a
    # source file's own directory, so the header must be listed explicitly
    # here or it's silently never moc'd (an "undefined reference to vtable
    # for ..." link error, not a moc/compile error).
    include/wrftools/point_inspector_dialog.hpp
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
    # Same source icon as the Linux AppImage (resources/wrftools.png),
    # converted to .icns - CMake copies a MACOSX_PACKAGE_LOCATION source
    # into the bundle's Contents/Resources for us.
    target_sources(wrftools_cpp PRIVATE resources/wrftools.icns)
    set_source_files_properties(resources/wrftools.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
    )
    # Without MACOSX_BUNDLE_GUI_IDENTIFIER, the bundle's CFBundleIdentifier
    # is left blank. On macOS 26 (Tahoe), a blank bundle identifier makes
    # NSOpenPanel/NSSavePanel silently fail to display - QFileDialog's
    # native calls return with no dialog, no error, and no log output
    # (https://forum.qt.io/topic/162825/).
    set_target_properties(wrftools_cpp PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER org.wrftools.wrftools
        MACOSX_BUNDLE_ICON_FILE wrftools.icns
    )
elseif(WIN32)
    # Without this, add_executable() links wrftools.exe as a console-
    # subsystem app, so Windows always opens a console window behind the
    # Qt GUI on launch (Linux/macOS have no such distinction).
    set_target_properties(wrftools_cpp PROPERTIES WIN32_EXECUTABLE TRUE)
    # Same source icon as the Linux AppImage/macOS bundle (resources/
    # wrftools.png), converted to .ico and embedded via a .rc resource -
    # that's the only way an .exe picks up an icon on Windows.
    target_sources(wrftools_cpp PRIVATE resources/wrftools.rc)
endif()

# Headless worker the Reproject tab launches as a separate process (see
# reproject_worker.cpp's own comment for why) - links only wrftools_core,
# not wrftools_ui/Widgets, so it never shares GDAL/HDF5's GUI-thread
# affinity with the main app process.
add_executable(wrftools_reproject_worker src/reproject_worker.cpp)
target_link_libraries(wrftools_reproject_worker PRIVATE wrftools_core Qt6::Core)
target_compile_options(wrftools_reproject_worker PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS wrftools_reproject_worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(APPLE)
    # workerExecutablePath() (reproject_form.cpp) looks for this binary
    # next to the main executable via QCoreApplication::applicationDirPath()
    # - which resolves to Contents/MacOS/ inside a .app bundle, not the
    # top-level build directory add_executable() links it into by default.
    # Without this copy, the Reproject tab's "launch worker" call fails to
    # find it in every plain `cmake --build` (only build-portable-macos.sh's
    # separate packaging step did this copy before now).
    add_custom_command(TARGET wrftools_cpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:wrftools_reproject_worker>"
            "$<TARGET_BUNDLE_CONTENT_DIR:wrftools_cpp>/MacOS/$<TARGET_FILE_NAME:wrftools_reproject_worker>"
        COMMENT "Copying wrftools_reproject_worker into the app bundle"
    )
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
