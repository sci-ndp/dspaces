#
#  general cmake flags:
#    -DCMAKE_INSTALL_PREFIX=/usr/local     -- the prefix for installing
#    -DCMAKE_BUILD_TYPE=type               -- type can be Debug, Release, ...
#    -DCMAKE_PREFIX_PATH=/dir              -- external packages
#
#     note that CMAKE_PREFIX_PATH can be a list of directories:
#      -DCMAKE_PREFIX_PATH='/dir1;/dir2;/dir3'
#

cmake_minimum_required (VERSION 3.16)
project (dspaces VERSION 2.2.0 LANGUAGES C)

cmake_policy(SET CMP0078 NEW)
cmake_policy(SET CMP0086 NEW)

#enable_testing ()

include(GNUInstallDirs)

if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY
    ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR})
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
    ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR})
endif()
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY
    ${PROJECT_BINARY_DIR}/${CMAKE_INSTALL_BINDIR})
endif()

include(CMakeDependentOption)
get_property(SHARED_LIBS_SUPPORTED GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS)
cmake_dependent_option(BUILD_SHARED_LIBS
  "Build shared libraries (so/dylib/dll)." ${SHARED_LIBS_SUPPORTED}
  "SHARED_LIBS_SUPPORTED" OFF
)
mark_as_advanced(BUILD_SHARED_LIBS)
if((NOT BUILD_SHARED_LIBS) AND (NOT DEFINED CMAKE_POSITION_INDEPENDENT_CODE))
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

option(ENABLE_TESTS    "Build tests" OFF)
set(DSPACES_PYTHON_BINDINGS AUTO CACHE STRING  "Build Python bindings")
set_property(CACHE DSPACES_PYTHON_BINDINGS PROPERTY STRINGS "ON;TRUE;AUTO;OFF;FALSE")

# add our cmake module directory to the path
set (CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH}
     "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# link shared lib with full rpath
set (CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set (CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

set (CMAKE_PREFIX_PATH "" CACHE STRING "External dependencies path")
set (BUILD_SHARED_LIBS "OFF" CACHE BOOL "Build a shared library")

# packages we depend on
include (xpkg-import)
xpkg_import_module (margo REQUIRED margo)
xpkg_import_module (liblz4 REQUIRED liblz4)

include(python_bindings)

find_package(MPI COMPONENTS REQUIRED)

set(DSPACES_USE_OPENMP ON CACHE STRING "Use OpenMP for operations")
mark_as_advanced(DSPACES_USE_OPENMP)
find_package(OpenMP)
if(OPENMP_FOUND AND DSPACES_USE_OPENMP)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    add_definitions(-DOPS_USE_OPENMP)
endif()
find_package(CURL)
if(CURL_FOUND)
    add_definitions(-DDSPACES_HAVE_CURL)
endif()

include(CheckLanguage)
check_language(Fortran)
if(CMAKE_Fortran_COMPILER)
    enable_language(Fortran)
endif()
if(CMAKE_Fortran_COMPILER_LOADED)
    include(CheckFortranCompilerFlag)
    set(DSPACES_HAVE_FORTRAN TRUE)
    check_fortran_compiler_flag("-fallow-argument-mismatch" FORTRAN_HAS_ARG_MISMATCH_FLAG)
    if(FORTRAN_HAS_ARG_MISMATCH_FLAG)
        SET(CMAKE_Fortran_FLAGS "${CMAKE_CXX_FLAGS} -fallow-argument-mismatch -Wno-lto-type-mismatch")
    endif()
endif()

add_subdirectory(src)
add_subdirectory(include)
add_subdirectory(bindings)
add_subdirectory(modules)
if(${ENABLE_TESTS})
  enable_testing()
  add_subdirectory(tests)
endif(${ENABLE_TESTS})
if(${ENABLE_EXAMPLES})
    configure_file(
        ${PROJECT_SOURCE_DIR}/examples/opts.mk.in
        ${PROJECT_BINARY_DIR}/examples/opts.mk
        @ONLY
    )
    file(GLOB EXAMPLE_DIRS ${PROJECT_SOURCE_DIR}/examples/ex*)
    file(COPY ${PROJECT_SOURCE_DIR}/examples/Makefile ${EXAMPLE_DIRS} DESTINATION ${PROJECT_BINARY_DIR}/examples)
endif(${ENABLE_EXAMPLES})
