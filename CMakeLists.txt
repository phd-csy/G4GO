cmake_minimum_required(VERSION 3.24)

project(G4GO LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

set(CLI11_BUILD_DOCS OFF CACHE BOOL "Disable CLI11 documentation" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "Disable CLI11 examples" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "Disable CLI11 tests" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "Disable CLI11 installation" FORCE)
set(CLI11_MODULES OFF CACHE BOOL "Disable CLI11 modules" FORCE)
set(CLI11_PRECOMPILED OFF CACHE BOOL "Use CLI11 header-only mode" FORCE)
FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.7.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(CLI11)

FetchContent_Declare(
    optix_dev
    GIT_REPOSITORY https://github.com/NVIDIA/optix-dev.git
    GIT_TAG v9.1.0
    GIT_SHALLOW TRUE
)

include(CTest)

option(G4GO_BUILD_UIVIS "Build with Geant4 UI and visualization support" ON)
option(G4GO_ENABLE_OPTIX "Try to build the CUDA/OptiX optical backend" ON)
option(G4GO_ENABLE_FORMAT_TARGETS "Add clang-format targets" ON)
option(G4GO_ENABLE_CLANG_TIDY "Enable clang-tidy during compilation" OFF)

set(G4GO_CUDA_ARCHITECTURE "120"
    CACHE STRING "CUDA architecture used by the OptiX backend")

set(G4GO_OPTIX_AVAILABLE OFF)
if(G4GO_ENABLE_OPTIX)
    FetchContent_MakeAvailable(optix_dev)

    include(CheckLanguage)
    check_language(CUDA)

    if(CMAKE_CUDA_COMPILER)
        find_package(CUDAToolkit 12.0 QUIET)
        if(CUDAToolkit_FOUND)
            enable_language(CUDA)
            find_program(G4GO_BIN2C_EXECUTABLE
                NAMES bin2c
                HINTS ${CUDAToolkit_BIN_DIR})

            if(G4GO_BIN2C_EXECUTABLE AND
               EXISTS ${optix_dev_SOURCE_DIR}/include/optix.h AND
               EXISTS ${optix_dev_SOURCE_DIR}/include/optix_stubs.h AND
               EXISTS ${optix_dev_SOURCE_DIR}/include/optix_function_table_definition.h)
                set(G4GO_OPTIX_AVAILABLE ON)
                message(STATUS "G4GO OptiX build: enabled")
            else()
                message(STATUS
                    "G4GO OptiX build dependencies are incomplete; using Geant4 fallback")
            endif()
        else()
            message(STATUS
                "G4GO OptiX build dependencies are incomplete; using Geant4 fallback")
        endif()
    else()
        message(STATUS
            "CUDA compiler was not detected; using Geant4 fallback")
    endif()
else()
    message(STATUS "OptiX build disabled by G4GO_ENABLE_OPTIX; using Geant4 fallback")
endif()

if(G4GO_BUILD_UIVIS)
    find_package(Geant4 11.4.2 REQUIRED ui_all vis_all)
else()
    find_package(Geant4 11.4.2 REQUIRED)
endif()

include(${Geant4_USE_FILE})

if(G4GO_ENABLE_CLANG_TIDY)
    find_program(G4GO_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
endif()

function(g4go_configure_cpp_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target}
        PUBLIC ${PROJECT_SOURCE_DIR}/include)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(G4GO_ENABLE_CLANG_TIDY)
        set_target_properties(${target} PROPERTIES
            CXX_CLANG_TIDY "${G4GO_CLANG_TIDY_EXECUTABLE}")
    endif()
endfunction()

file(GLOB G4GO_OPTICAL_CORE_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/optical/*.cpp"
    "${PROJECT_SOURCE_DIR}/include/g4go/optical/*.hpp"
)

add_library(g4go_optical_core STATIC
    ${G4GO_OPTICAL_CORE_FILES}
)
g4go_configure_cpp_target(g4go_optical_core)
target_include_directories(g4go_optical_core
    PUBLIC ${Geant4_INCLUDE_DIRS})
target_link_libraries(g4go_optical_core
    PUBLIC ${Geant4_LIBRARIES})

if(G4GO_OPTIX_AVAILABLE)
    set(G4GO_OPTIX_GENERATED_DIR ${PROJECT_BINARY_DIR}/generated)
    set(G4GO_OPTIX_IR ${G4GO_OPTIX_GENERATED_DIR}/g4go_optical.optixir)
    set(G4GO_OPTIX_IR_HEADER ${G4GO_OPTIX_GENERATED_DIR}/g4go_optix_ir.h)
    file(GLOB G4GO_OPTIX_DEVICE_FILES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/optical/optix/*.cu"
        "${PROJECT_SOURCE_DIR}/src/optical/optix/*.cuh"
    )
    file(GLOB G4GO_OPTIX_HOST_FILES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/optical/optix/*.cpp"
        "${PROJECT_SOURCE_DIR}/src/optical/optix/*.cuh"
        "${PROJECT_SOURCE_DIR}/include/g4go/optical/optix/*.hpp"
    )

    add_custom_command(
        OUTPUT ${G4GO_OPTIX_IR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${G4GO_OPTIX_GENERATED_DIR}
        COMMAND ${CMAKE_CUDA_COMPILER}
                --optix-ir
                --expt-relaxed-constexpr
                -std=c++20
                -arch=compute_${G4GO_CUDA_ARCHITECTURE}
                -I${PROJECT_SOURCE_DIR}/include
                -I${optix_dev_SOURCE_DIR}/include
                -c ${PROJECT_SOURCE_DIR}/src/optical/optix/OptiXTransportDevice.cu
                -o ${G4GO_OPTIX_IR}
        DEPENDS
            ${G4GO_OPTIX_DEVICE_FILES}
        COMMENT "Compiling OptiX device program"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${G4GO_OPTIX_IR_HEADER}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${G4GO_OPTIX_IR}
                -DOUTPUT=${G4GO_OPTIX_IR_HEADER}
                -DBIN2C=${G4GO_BIN2C_EXECUTABLE}
                -P ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        DEPENDS
            ${G4GO_OPTIX_IR}
            ${PROJECT_SOURCE_DIR}/cmake/EmbedBinary.cmake
        COMMENT "Embedding OptiX device program"
        VERBATIM
    )

    add_custom_target(g4go_optix_module DEPENDS ${G4GO_OPTIX_IR_HEADER})

    add_library(g4go_optical_optix STATIC
        ${G4GO_OPTIX_HOST_FILES}
        ${G4GO_OPTIX_IR_HEADER}
    )
    g4go_configure_cpp_target(g4go_optical_optix)
    add_dependencies(g4go_optical_optix g4go_optix_module)
    target_include_directories(g4go_optical_optix PRIVATE
        ${optix_dev_SOURCE_DIR}/include
        ${G4GO_OPTIX_GENERATED_DIR})
    target_compile_definitions(g4go_optical_optix PRIVATE G4GO_ENABLE_OPTIX)
    target_link_libraries(g4go_optical_optix
        PUBLIC g4go_optical_core
        PRIVATE CUDA::cudart CUDA::cuda_driver)
endif()

file(GLOB G4GO_OPTICAL_GEANT4_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/optical/geant4/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/optical/geant4/*.hpp"
    "${PROJECT_SOURCE_DIR}/include/g4go/optical/geant4/*.hpp"
)

add_library(g4go_optical_geant4 STATIC
    ${G4GO_OPTICAL_GEANT4_FILES}
)
g4go_configure_cpp_target(g4go_optical_geant4)
target_link_libraries(g4go_optical_geant4
    PUBLIC g4go_optical_core
    PRIVATE ${Geant4_LIBRARIES})

if(G4GO_OPTIX_AVAILABLE)
    target_compile_definitions(g4go_optical_geant4 PRIVATE G4GO_ENABLE_OPTIX)
    target_include_directories(g4go_optical_geant4 PRIVATE
        ${PROJECT_SOURCE_DIR}/src/optical/optix)
    target_link_libraries(g4go_optical_geant4 PRIVATE
        g4go_optical_optix CUDA::cudart)
endif()

file(GLOB G4GO_DETECTOR_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/detector/*.cpp"
    "${PROJECT_SOURCE_DIR}/include/g4go/detector/*.hpp"
)

add_library(g4go_detector STATIC
    ${G4GO_DETECTOR_FILES}
)
g4go_configure_cpp_target(g4go_detector)
target_link_libraries(g4go_detector PRIVATE ${Geant4_LIBRARIES})

file(GLOB G4GO_SIMULATION_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/simulation/*.cpp"
    "${PROJECT_SOURCE_DIR}/include/g4go/simulation/*.hpp"
)

add_library(g4go_simulation STATIC
    ${G4GO_SIMULATION_FILES}
)
g4go_configure_cpp_target(g4go_simulation)
target_link_libraries(g4go_simulation
    PUBLIC g4go_detector g4go_optical_geant4
    PRIVATE ${Geant4_LIBRARIES})

file(GLOB G4GO_APPLICATION_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
)

add_executable(g4go ${G4GO_APPLICATION_FILES})
g4go_configure_cpp_target(g4go)
target_link_libraries(g4go PRIVATE CLI11::CLI11 g4go_simulation ${Geant4_LIBRARIES})

if(G4GO_BUILD_UIVIS)
    target_compile_definitions(g4go PRIVATE G4GO_USE_UIVIS)
endif()

set(G4GO_SCRIPT_DIR "${PROJECT_SOURCE_DIR}/scripts")
file(GLOB G4GO_SCRIPT_FILES CONFIGURE_DEPENDS
    "${G4GO_SCRIPT_DIR}/*"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${G4GO_SCRIPT_FILES}
)
file(COPY "${G4GO_SCRIPT_DIR}" DESTINATION "${PROJECT_BINARY_DIR}")

if(G4GO_ENABLE_FORMAT_TARGETS)
    find_program(G4GO_CLANG_FORMAT_EXECUTABLE NAMES clang-format)

    if(G4GO_CLANG_FORMAT_EXECUTABLE)
        file(GLOB_RECURSE G4GO_FORMAT_FILES CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/src/*.cpp"
            "${PROJECT_SOURCE_DIR}/src/*.hpp"
            "${PROJECT_SOURCE_DIR}/src/*.cu"
            "${PROJECT_SOURCE_DIR}/src/*.cuh"
            "${PROJECT_SOURCE_DIR}/include/*.hpp"
            "${PROJECT_SOURCE_DIR}/include/*.cuh"
            "${PROJECT_SOURCE_DIR}/test/*.cpp"
            "${PROJECT_SOURCE_DIR}/test/*.cxx"
            "${PROJECT_SOURCE_DIR}/test/*.C"
        )

        add_custom_target(g4go-format
            COMMAND ${G4GO_CLANG_FORMAT_EXECUTABLE}
                    --style=file
                    -i
                    ${G4GO_FORMAT_FILES}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT "Formatting G4GO C++ sources"
            VERBATIM
        )

        add_custom_target(g4go-format-check
            COMMAND ${G4GO_CLANG_FORMAT_EXECUTABLE}
                    --style=file
                    --dry-run
                    --Werror
                    ${G4GO_FORMAT_FILES}
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            COMMENT "Checking G4GO C++ formatting"
            VERBATIM
        )
    else()
        message(STATUS "clang-format not found; format targets are unavailable")
    endif()
endif()

install(TARGETS g4go RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    add_executable(g4go_optical_boundary_tests
        test/TestOpticalBoundaryPhysics.cxx)
    g4go_configure_cpp_target(g4go_optical_boundary_tests)
    target_include_directories(g4go_optical_boundary_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/optical/optix)
    add_test(NAME g4go_optical_boundary
             COMMAND $<TARGET_FILE:g4go_optical_boundary_tests>)
    set_tests_properties(g4go_optical_boundary PROPERTIES
        LABELS "unit;cpu"
        TIMEOUT 30)

    set(G4GO_TEST_RUNTIME_FILES
        "${PROJECT_SOURCE_DIR}/test/TestOpticalTransport.cxx"
        "${PROJECT_SOURCE_DIR}/test/regression_test.sh"
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${G4GO_TEST_RUNTIME_FILES}
    )
    foreach(test_runtime_file IN LISTS G4GO_TEST_RUNTIME_FILES)
        get_filename_component(test_runtime_name "${test_runtime_file}" NAME)
        configure_file(
            "${test_runtime_file}"
            "${PROJECT_BINARY_DIR}/test/${test_runtime_name}"
            COPYONLY)
    endforeach()

    add_test(NAME g4go_smoke_auto
             COMMAND $<TARGET_FILE:g4go>
                     ${PROJECT_BINARY_DIR}/scripts/run_optical_test.mac)
    set_tests_properties(g4go_smoke_auto PROPERTIES
        LABELS "smoke;integration;auto"
        TIMEOUT 60
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})

    find_program(G4GO_BASH_EXECUTABLE NAMES bash REQUIRED)
    add_test(NAME g4go_noptpho_regression
             COMMAND ${G4GO_BASH_EXECUTABLE}
                     ${PROJECT_BINARY_DIR}/test/regression_test.sh
                     --build-dir ${PROJECT_BINARY_DIR})
    set_tests_properties(g4go_noptpho_regression PROPERTIES
        LABELS "regression;cpu;gpu;slow"
        RUN_SERIAL TRUE
        TIMEOUT 600
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})
endif()
