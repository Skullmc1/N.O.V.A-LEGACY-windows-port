cmake_minimum_required(VERSION 3.20)
project(nova_loader CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Keep symbols in optimised builds so the built-in sampler (--profile) can name functions.
set(NOVA_PDB ON)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Zi")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

set(EXT ${CMAKE_SOURCE_DIR}/externals)

# dynarmic only needs Boost's headers; point its find_package at our stub config.
set(Boost_DIR ${CMAKE_SOURCE_DIR}/cmake/boost)
set(NOVA_BOOST_INCLUDE ${EXT}/boost_1_86_0)

set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
add_subdirectory(${EXT}/dynarmic dynarmic EXCLUDE_FROM_ALL)

add_executable(nova
    src/main.cpp
    src/guest.cpp
    src/libc.cpp
    src/stdio.cpp
    src/pthread.cpp
    src/jni.cpp
    src/android.cpp
    src/gles.cpp
    src/misc.cpp
    src/audio.cpp
    src/controls.cpp
    src/interp.cpp
    src/hle.cpp
    src/packs.cpp
)
target_include_directories(nova PRIVATE
    ${EXT}/khronos
    ${EXT}/SDL2-2.32.10/include
)
target_compile_definitions(nova PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(nova PRIVATE dynarmic ${EXT}/SDL2-2.32.10/lib/x64/SDL2.lib synchronization dbghelp psapi)
set_target_properties(nova PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin
)
