cmake_minimum_required(VERSION 3.5.1)
project(bladeRFToFIFO C)

#Using technique from Marcus D. Hanwell on https://blog.kitware.com/cmake-and-the-default-build-type/ to set default build type
set(default_build_type "Release")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
    set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
            STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

set(CMAKE_C_STANDARD 11)

find_package(Threads)
find_library(LIBRT rt)
if(NOT LIBRT)
    message(FATAL_ERROR "librt not found")
endif()
find_library(LIBM m)
if(NOT LIBM)
    message(FATAL_ERROR "stdlib Math not found")
endif()

#With help from https://stackoverflow.com/questions/11961303/cmake-which-package-name-to-pass-to-find-package
#With help from https://stackoverflow.com/questions/49737854/how-to-use-ld-library-path-in-cmakelists
#     on how to get find library to search LD_LIBRARY_PATH
# https://cmake.org/cmake/help/latest/command/find_library.html
if(DEFINED ENV{LD_LIBRARY_PATH})
    string(REPLACE ":" ";" LD_LIBRARY_PATH_CMAKE_LIST $ENV{LD_LIBRARY_PATH})
    find_library(LIB_BLADERF bladeRF PATHS ${LD_LIBRARY_PATH_CMAKE_LIST})
else()
    find_library(LIB_BLADERF bladeRF)
endif()

if(DEFINED ENV{C_INCLUDE_PATH})
    string(REPLACE ":" ";" C_INCLUDE_PATH_CMAKE_LIST $ENV{C_INCLUDE_PATH})
    find_path(INCLUDE_BLADERF libbladeRF.h PATHS ${C_INCLUDE_PATH_CMAKE_LIST})
else()
    find_path(INCLUDE_BLADERF libbladeRF.h)
endif()

if(NOT LIB_BLADERF)
    message(FATAL_ERROR "bladeRF not found")
endif()
if(NOT INCLUDE_BLADERF)
    message(FATAL_ERROR "bladeRF include not found")
endif()
include_directories(${INCLUDE_BLADERF})
message(STATUS "Found bladeRF - lib: ${LIB_BLADERF}, include: ${INCLUDE_BLADERF}/libbladeRF.h")

set(COMMON_SRCS
        src/depends/BerkeleySharedMemoryFIFO.c
        src/depends/BerkeleySharedMemoryFIFO.h
        src/rxThread.c
        src/rxThread.h
        src/txThread.c
        src/txThread.h
        src/helpers.c)

add_executable(bladeRFToFIFO src/main.c ${COMMON_SRCS})
target_link_libraries(bladeRFToFIFO ${CMAKE_THREAD_LIBS_INIT} ${LIBRT} ${LIBM} ${LIB_BLADERF})
