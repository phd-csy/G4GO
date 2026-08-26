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

function(g4go_configure_cpp_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_include_directories(${target}
        PUBLIC ${PROJECT_SOURCE_DIR}/include)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

set(G4GO_OPTICAL_CORE_SOURCES
    src/optical/Geometry.cpp
    src/optical/Scene.cpp
)

set(G4GO_OPTICAL_CORE_HEADERS
    include/g4go/optical/Geometry.hpp
    include/g4go/optical/Random.hpp
    include/g4go/optical/Scene.hpp
    include/g4go/optical/Transport.hpp
    include/g4go/optical/Types.hpp
)

add_library(g4go_optical_core STATIC
    ${G4GO_OPTICAL_CORE_SOURCES}
    ${G4GO_OPTICAL_CORE_HEADERS}
)
g4go_configure_cpp_target(g4go_optical_core)

if(G4GO_OPTIX_AVAILABLE)
    set(G4GO_OPTIX_GENERATED_DIR ${PROJECT_BINARY_DIR}/generated)
    set(G4GO_OPTIX_IR ${G4GO_OPTIX_GENERATED_DIR}/g4go_optical.optixir)
    set(G4GO_OPTIX_IR_HEADER ${G4GO_OPTIX_GENERATED_DIR}/g4go_optix_ir.h)

    add_custom_command(
        OUTPUT ${G4GO_OPTIX_IR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${G4GO_OPTIX_GENERATED_DIR}
        COMMAND ${CMAKE_CUDA_COMPILER}
                --optix-ir
                -std=c++20
                -arch=compute_${G4GO_CUDA_ARCHITECTURE}
                -I${PROJECT_SOURCE_DIR}/include
                -I${optix_dev_SOURCE_DIR}/include
                -c ${PROJECT_SOURCE_DIR}/src/optical/optix/OptixTransportDevice.cu
                -o ${G4GO_OPTIX_IR}
        DEPENDS
            src/optical/optix/OptixTransportDevice.cu
            src/optical/optix/OptixTransportTypes.cuh
            src/optical/optix/BoundaryPhysics.cuh
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
        src/optical/optix/OptixTransportHost.cpp
        include/g4go/optical/optix/Transport.hpp
        src/optical/optix/OptixTransportTypes.cuh
        src/optical/optix/BoundaryPhysics.cuh
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

add_library(g4go_optical_geant4 STATIC
    src/optical/geant4/EventBridge.cpp
    src/optical/geant4/OpticalBatchService.cpp
    src/optical/geant4/SceneExporter.cpp
    src/optical/geant4/SceneExporter.hpp
    src/optical/geant4/StackingAction.cpp
    include/g4go/optical/geant4/EventBridge.hpp
    include/g4go/optical/geant4/OpticalBatchService.hpp
    include/g4go/optical/geant4/StackingAction.hpp
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

add_library(g4go_detector STATIC
    src/detector/DetectorConstruction.cpp
    src/detector/ScintillatorSD.cpp
    src/detector/SensorSD.cpp
    include/g4go/detector/DetectorConstruction.hpp
    include/g4go/detector/ScintillatorHit.hpp
    include/g4go/detector/ScintillatorSD.hpp
    include/g4go/detector/SensorHit.hpp
    include/g4go/detector/SensorSD.hpp
)
g4go_configure_cpp_target(g4go_detector)
target_link_libraries(g4go_detector PRIVATE ${Geant4_LIBRARIES})

add_library(g4go_simulation STATIC
    src/simulation/ActionInitialization.cpp
    src/simulation/EventAction.cpp
    src/simulation/EventOutput.cpp
    src/simulation/PrimaryGeneratorAction.cpp
    src/simulation/RunAction.cpp
    include/g4go/simulation/ActionInitialization.hpp
    include/g4go/simulation/EventAction.hpp
    include/g4go/simulation/EventOutput.hpp
    include/g4go/simulation/PrimaryGeneratorAction.hpp
    include/g4go/simulation/RunAction.hpp
)
g4go_configure_cpp_target(g4go_simulation)
target_link_libraries(g4go_simulation
    PUBLIC g4go_detector g4go_optical_geant4
    PRIVATE ${Geant4_LIBRARIES})

add_executable(g4go src/main.cpp)
g4go_configure_cpp_target(g4go)
target_link_libraries(g4go PRIVATE CLI11::CLI11 g4go_simulation ${Geant4_LIBRARIES})

if(G4GO_BUILD_UIVIS)
    target_compile_definitions(g4go PRIVATE G4GO_USE_UIVIS)
endif()

if(G4GO_ENABLE_CLANG_TIDY)
    find_program(G4GO_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
    set_target_properties(g4go PROPERTIES
        CXX_CLANG_TIDY "${G4GO_CLANG_TIDY_EXECUTABLE}")
endif()

set(G4GO_SCRIPT_DIR ${PROJECT_SOURCE_DIR}/scripts)

file(GLOB G4GO_MACRO_FILES CONFIGURE_DEPENDS
    ${G4GO_SCRIPT_DIR}/*.mac
)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${G4GO_MACRO_FILES}
)

foreach(macroFile IN LISTS G4GO_MACRO_FILES)
    get_filename_component(macroName "${macroFile}" NAME)
    set(macroOutput "${PROJECT_BINARY_DIR}/${macroName}")
    configure_file("${macroFile}" "${macroOutput}" COPYONLY)
endforeach()

if(G4GO_ENABLE_FORMAT_TARGETS)
    find_program(G4GO_CLANG_FORMAT_EXECUTABLE NAMES clang-format)

    if(G4GO_CLANG_FORMAT_EXECUTABLE)
        set(G4GO_FORMAT_FILES
            src/main.cpp
            ${G4GO_OPTICAL_CORE_SOURCES}
            ${G4GO_OPTICAL_CORE_HEADERS}
            src/optical/geant4/EventBridge.cpp
            src/optical/geant4/OpticalBatchService.cpp
            src/optical/geant4/SceneExporter.cpp
            src/optical/geant4/SceneExporter.hpp
            src/optical/geant4/StackingAction.cpp
            include/g4go/optical/geant4/EventBridge.hpp
            include/g4go/optical/geant4/OpticalBatchService.hpp
            include/g4go/optical/geant4/StackingAction.hpp
            src/optical/optix/OptixTransportHost.cpp
            src/optical/optix/OptixTransportDevice.cu
            src/optical/optix/OptixTransportTypes.cuh
            src/optical/optix/BoundaryPhysics.cuh
            include/g4go/optical/optix/Transport.hpp
            test/OpticalBoundaryPhysicsTests.cpp
            src/detector/DetectorConstruction.cpp
            src/detector/ScintillatorSD.cpp
            src/detector/SensorSD.cpp
            include/g4go/detector/DetectorConstruction.hpp
            include/g4go/detector/ScintillatorHit.hpp
            include/g4go/detector/ScintillatorSD.hpp
            include/g4go/detector/SensorHit.hpp
            include/g4go/detector/SensorSD.hpp
            src/simulation/ActionInitialization.cpp
            src/simulation/EventAction.cpp
            src/simulation/EventOutput.cpp
            src/simulation/PrimaryGeneratorAction.cpp
            src/simulation/RunAction.cpp
            include/g4go/simulation/ActionInitialization.hpp
            include/g4go/simulation/EventAction.hpp
            include/g4go/simulation/EventOutput.hpp
            include/g4go/simulation/PrimaryGeneratorAction.hpp
            include/g4go/simulation/RunAction.hpp
            test/TestOpticalTransport.C
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
        test/OpticalBoundaryPhysicsTests.cpp)
    g4go_configure_cpp_target(g4go_optical_boundary_tests)
    target_include_directories(g4go_optical_boundary_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/optical/optix)
    add_test(NAME g4go_optical_boundary
             COMMAND $<TARGET_FILE:g4go_optical_boundary_tests>)

    add_test(NAME g4go_smoke_auto
             COMMAND $<TARGET_FILE:g4go>
                     ${PROJECT_BINARY_DIR}/run_optical_smoke.mac)
    set_tests_properties(g4go_smoke_auto PROPERTIES
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})

    find_program(G4GO_BASH_EXECUTABLE NAMES bash REQUIRED)
    add_test(NAME g4go_noptpho_regression
             COMMAND ${G4GO_BASH_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/test/regression_test.sh
                     --build-dir ${PROJECT_BINARY_DIR})
    set_tests_properties(g4go_noptpho_regression PROPERTIES
        LABELS "regression;gpu"
        RUN_SERIAL TRUE
        TIMEOUT 600
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})
endif()
