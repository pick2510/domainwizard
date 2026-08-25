# Bundles the built wrftools_cpp executable into a self-contained,
# relocatable directory: the binary, every shared library it (transitively)
# needs that isn't reasonably assumed present on any Linux machine, the Qt
# plugins it dlopen()s at runtime (platform + TLS backend + image formats -
# none of these are linked, so they're invisible to normal dependency-
# closure resolution, and each pulls in its own further dependencies, e.g.
# the TLS backend needs libssl/libcrypto that nothing else here may need),
# and GDAL/PROJ's data files. patchelf then rewrites RPATHs so the bundle
# never looks outside itself.
#
# Deliberately NOT bundled: the dynamic loader/libc/libpthread/libdl/librt/
# libm/libgcc_s/libstdc++ family. These are tied to the kernel/ABI of the
# machine that runs the bundle - bundling them (as opposed to depending on
# whatever the target machine already has) is what actually needs the old-
# glibc-baseline Docker build build-portable.sh uses for the Python side.
# This script instead promises portability across machines with a
# comparably-recent-or-newer glibc than the build machine, which covers
# "runs on other Debian/Ubuntu machines" without that extra infrastructure.
#
# Invoked via: cmake -DEXECUTABLE=<path> -DOUTPUT_DIR=<dir> -P cmake/bundle_linux.cmake
# (the -D flags must come BEFORE -P - cmake ignores them placed after it)

if(NOT EXECUTABLE OR NOT EXISTS "${EXECUTABLE}")
    message(FATAL_ERROR "Pass -DEXECUTABLE=<path to the built wrftools binary>")
endif()
if(NOT OUTPUT_DIR)
    message(FATAL_ERROR "Pass -DOUTPUT_DIR=<directory to create the bundle in>")
endif()

find_program(PATCHELF_EXECUTABLE patchelf)
if(NOT PATCHELF_EXECUTABLE)
    message(FATAL_ERROR "patchelf not found - install it (e.g. `apt install patchelf`) first.")
endif()

# Libraries assumed present (correctly, at an ABI-compatible version) on any
# target Linux machine - the loader itself, and the small set of libraries
# every C/C++ program on the system already depends on transitively.
set(EXCLUDED_PATTERNS
    "^linux-vdso\\.so"
    "^ld-linux"
    "^libc\\.so"
    "^libm\\.so"
    "^libdl\\.so"
    "^libpthread\\.so"
    "^librt\\.so"
    "^libgcc_s\\.so"
    "^libstdc\\+\\+\\.so"
)

file(MAKE_DIRECTORY "${OUTPUT_DIR}/bin")
file(MAKE_DIRECTORY "${OUTPUT_DIR}/lib")
file(MAKE_DIRECTORY "${OUTPUT_DIR}/share/gdal")
file(MAKE_DIRECTORY "${OUTPUT_DIR}/share/proj")

get_filename_component(EXE_NAME "${EXECUTABLE}" NAME)
file(COPY "${EXECUTABLE}" DESTINATION "${OUTPUT_DIR}/bin")
set(BUNDLED_EXE "${OUTPUT_DIR}/bin/${EXE_NAME}")

# --- Locate the Qt plugin directory, from the ACTUAL libQt6Core.so this
# build links against - not a hardcoded prefix, since a Debian/Ubuntu
# multiarch install keeps plugins under <libdir>/qt6/plugins while a
# from-source/aqt install keeps them under <prefix>/plugins (<libdir>/..) ---
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${BUNDLED_EXE}"
    RESOLVED_DEPENDENCIES_VAR exe_deps
    UNRESOLVED_DEPENDENCIES_VAR exe_unresolved
)
if(exe_unresolved)
    message(WARNING "Could not resolve: ${exe_unresolved}")
endif()

set(qt_core_lib "")
foreach(dep_path ${exe_deps})
    if(dep_path MATCHES "libQt6Core\\.so")
        set(qt_core_lib "${dep_path}")
    endif()
endforeach()
if(NOT qt_core_lib)
    message(FATAL_ERROR "Could not locate libQt6Core.so among resolved dependencies - is this actually a Qt6 build?")
endif()
get_filename_component(qt_libdir "${qt_core_lib}" DIRECTORY)
get_filename_component(qt_prefix "${qt_libdir}" DIRECTORY)

set(qt_plugins_dir "")
foreach(candidate "${qt_libdir}/qt6/plugins" "${qt_prefix}/plugins")
    if(EXISTS "${candidate}/platforms/libqxcb.so")
        set(qt_plugins_dir "${candidate}")
        break()
    endif()
endforeach()
if(NOT qt_plugins_dir)
    message(FATAL_ERROR "Could not find Qt's platforms/libqxcb.so plugin near ${qt_core_lib} - checked ${qt_libdir}/qt6/plugins and ${qt_prefix}/plugins.")
endif()
message(STATUS "Bundling Qt plugins from ${qt_plugins_dir}")

# platforms: how Qt draws a window at all (xcb) / runs headless (offscreen,
# used by this project's own test suite). tls: QNetworkAccessManager's
# HTTPS backend for the OSM tile fetches - without it, every tile request
# silently fails ("No functional TLS backend was found"). imageformats:
# whatever's present, for broader QImage::load() format support.
set(qt_plugin_files
    "${qt_plugins_dir}/platforms/libqxcb.so"
    "${qt_plugins_dir}/platforms/libqoffscreen.so"
    "${qt_plugins_dir}/tls/libqopensslbackend.so"
    "${qt_plugins_dir}/tls/libqcertonlybackend.so"
)
if(EXISTS "${qt_plugins_dir}/imageformats")
    file(GLOB image_plugins "${qt_plugins_dir}/imageformats/*.so")
    list(APPEND qt_plugin_files ${image_plugins})
endif()

# --- Shared-library dependency closure, over the executable AND the Qt
# plugins together (MODULES, not EXECUTABLES - they're dlopen'd shared
# objects, not programs) - a plugin can need libraries nothing else here
# does (the TLS backend needs libssl/libcrypto that GDAL/curl may or may
# not have already pulled in) ---
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${BUNDLED_EXE}"
    MODULES ${qt_plugin_files}
    RESOLVED_DEPENDENCIES_VAR resolved_deps
    UNRESOLVED_DEPENDENCIES_VAR unresolved_deps
)
if(unresolved_deps)
    message(WARNING "Could not resolve: ${unresolved_deps}")
endif()

set(bundled_lib_names "")
foreach(dep_path ${resolved_deps})
    get_filename_component(dep_name "${dep_path}" NAME)
    set(excluded FALSE)
    foreach(pattern ${EXCLUDED_PATTERNS})
        if(dep_name MATCHES "${pattern}")
            set(excluded TRUE)
        endif()
    endforeach()
    if(NOT excluded)
        # dep_path is typically the un-versioned SONAME symlink (e.g.
        # libz.so.1 -> libz.so.1.3.1) - file(COPY) preserves symlinks as
        # symlinks rather than resolving them, which would leave a dangling
        # link in the bundle since the versioned target never gets copied.
        # configure_file(... COPYONLY) copies the resolved file's actual
        # bytes and lets us name it exactly dep_name (the SONAME the
        # binary's DT_NEEDED entries reference), collapsing the symlink
        # chain into one real file at the name the loader looks for.
        file(REAL_PATH "${dep_path}" real_dep_path)
        configure_file("${real_dep_path}" "${OUTPUT_DIR}/lib/${dep_name}" COPYONLY)
        list(APPEND bundled_lib_names "${dep_name}")
    endif()
endforeach()
list(LENGTH bundled_lib_names bundled_count)
message(STATUS "Bundled ${bundled_count} shared libraries into ${OUTPUT_DIR}/lib")

# --- Copy the Qt plugin files themselves, preserving their platforms/tls/
# imageformats subdirectory layout (Qt's plugin loader keys off it) ---
set(bundled_plugin_relpaths "")
foreach(plugin_file ${qt_plugin_files})
    if(EXISTS "${plugin_file}")
        get_filename_component(plugin_name "${plugin_file}" NAME)
        get_filename_component(plugin_category_dir "${plugin_file}" DIRECTORY)
        get_filename_component(plugin_category "${plugin_category_dir}" NAME)
        file(MAKE_DIRECTORY "${OUTPUT_DIR}/lib/qt6/plugins/${plugin_category}")
        file(COPY "${plugin_file}" DESTINATION "${OUTPUT_DIR}/lib/qt6/plugins/${plugin_category}")
        list(APPEND bundled_plugin_relpaths "qt6/plugins/${plugin_category}/${plugin_name}")
    endif()
endforeach()

# qt.conf tells Qt where its plugins live relative to the executable,
# instead of the compiled-in build-machine prefix - this is what makes the
# xcb/offscreen platform plugin loadable at all from a relocated bundle.
file(WRITE "${OUTPUT_DIR}/bin/qt.conf" "[Paths]\nPlugins = ../lib/qt6/plugins\n")

# --- GDAL data / PROJ data (proj.db) ------------------------------------
# main.cpp's configureGdalData() looks for these at ../share/gdal and
# ../share/proj relative to the executable, i.e. exactly OUTPUT_DIR/share/*
# given the bundle's bin/ + share/ layout here.
execute_process(COMMAND gdal-config --datadir OUTPUT_VARIABLE gdal_datadir OUTPUT_STRIP_TRAILING_WHITESPACE)
if(gdal_datadir AND EXISTS "${gdal_datadir}")
    file(GLOB gdal_data_files "${gdal_datadir}/*")
    file(COPY ${gdal_data_files} DESTINATION "${OUTPUT_DIR}/share/gdal")
else()
    message(WARNING "gdal-config --datadir did not resolve - GDAL_DATA will not be bundled.")
endif()

find_file(PROJ_DB proj.db PATHS /usr/share/proj /usr/local/share/proj)
if(PROJ_DB)
    file(COPY "${PROJ_DB}" DESTINATION "${OUTPUT_DIR}/share/proj")
else()
    message(WARNING "proj.db not found - PROJ_DATA will not be bundled (CRS transforms will fail).")
endif()

# --- RPATH rewriting ------------------------------------------------------
execute_process(COMMAND "${PATCHELF_EXECUTABLE}" --set-rpath "\$ORIGIN/../lib" "${BUNDLED_EXE}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "patchelf failed on ${BUNDLED_EXE}")
endif()
foreach(lib_name ${bundled_lib_names})
    execute_process(COMMAND "${PATCHELF_EXECUTABLE}" --set-rpath "\$ORIGIN" "${OUTPUT_DIR}/lib/${lib_name}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(WARNING "patchelf failed on ${lib_name}")
    endif()
endforeach()
# A plugin lives two directories below lib/ (lib/qt6/plugins/<category>/),
# so it needs $ORIGIN/../../.. to reach the bundled libraries in lib/.
foreach(plugin_relpath ${bundled_plugin_relpaths})
    execute_process(COMMAND "${PATCHELF_EXECUTABLE}" --set-rpath "\$ORIGIN/../../.." "${OUTPUT_DIR}/lib/${plugin_relpath}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(WARNING "patchelf failed on ${plugin_relpath}")
    endif()
endforeach()

message(STATUS "Portable bundle ready: ${OUTPUT_DIR}")
