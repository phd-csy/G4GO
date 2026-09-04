#include "OptiXBoundaryPhysics.cuh"
#include "g4go/optical/PhotonTransport.hpp"
#include "g4go/optical/Scene.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using G4GO::Optical::DeviceVector3;
using G4GO::Optical::InvalidID;
using G4GO::Optical::Scene;
using G4GO::Optical::Surface;
using G4GO::Optical::SurfaceFinish;
using G4GO::Optical::SurfaceModel;
using G4GO::Optical::SurfaceType;
using G4GO::Optical::Volume;
namespace Physics = G4GO::Optical::BoundaryPhysics;

auto Require(bool condition, const std::string& message) -> void {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
auto RequireNear(float actual,
                 float expected,
                 float tolerance,
                 const std::string& message) -> void {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message + ": actual=" +
                                 std::to_string(actual) +
                                 ", expected=" + std::to_string(expected));
    }
}

auto TestNormalIncidence() -> void {
    const auto output{Physics::ComputeFresnel(
        {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, -1.0F}, 1.0F, 1.5F)};

    Require(!output.fTotalInternalReflection,
            "normal incidence must transmit");
    RequireNear(output.fTransmittance, 0.96F, 1.0e-5F,
                "normal-incidence transmittance");
    RequireNear(output.fReflectedDirection.at(2), -1.0F, 1.0e-6F,
                "normal-incidence reflected direction");
    RequireNear(output.fTransmittedDirection.at(2), 1.0F, 1.0e-6F,
                "normal-incidence transmitted direction");
    RequireNear(Physics::Dot(output.fReflectedDirection,
                             output.fReflectedPolarization),
                0.0F, 1.0e-6F,
                "reflected polarization must be transverse");
}

auto TestBrewsterAngle() -> void {
    const auto angle{std::atan(1.5F)};
    const DeviceVector3 direction{std::sin(angle), 0.0F, std::cos(angle)};
    const DeviceVector3 sDirection{0.0F, 1.0F, 0.0F};
    const auto pPolarization{Physics::Normalize(
        Physics::Cross(direction, sDirection))};
    const auto output{Physics::ComputeFresnel(
        direction, pPolarization, {0.0F, 0.0F, -1.0F}, 1.0F, 1.5F)};

    RequireNear(output.fTransmittance, 1.0F, 1.0e-5F,
                "P-polarized Brewster transmittance");
}

auto TestTotalInternalReflection() -> void {
    constexpr auto angle{50.0F * std::numbers::pi_v<float> / 180.0F};
    const auto output{Physics::ComputeFresnel(
        {std::sin(angle), 0.0F, std::cos(angle)}, {0.0F, 1.0F, 0.0F},
        {0.0F, 0.0F, -1.0F}, 1.5F, 1.0F)};

    Require(output.fTotalInternalReflection,
            "50 degree glass-to-air incidence must be TIR");
    RequireNear(output.fTransmittance, 0.0F, 1.0e-6F,
                "TIR transmittance");
    Require(output.fReflectedDirection.at(2) < 0.0F,
            "TIR direction must return to the incident medium");
    RequireNear(Physics::Dot(output.fReflectedDirection,
                             output.fReflectedPolarization),
                0.0F, 1.0e-6F,
                "TIR polarization must be transverse");
}

auto TestSurfaceProbabilities() -> void {
    Require(Physics::ClassifySurface(0.8F, 0.1F, true, true, 0.05F) ==
                Physics::SurfaceOutcome::SurfaceInteraction,
            "reflectivity must select surface interaction below R");
    Require(Physics::ClassifySurface(0.8F, 0.1F, true, true, 0.75F) ==
                Physics::SurfaceOutcome::SurfaceInteraction,
            "reflectivity must include the upper R threshold");
    Require(Physics::ClassifySurface(0.8F, 0.1F, true, true, 0.85F) ==
                Physics::SurfaceOutcome::DirectTransmit,
            "transmittance must occupy the interval after R");
    Require(Physics::ClassifySurface(0.8F, 0.1F, true, true, 0.95F) ==
                Physics::SurfaceOutcome::Absorb,
            "remaining surface probability must absorb");
    Require(Physics::ClassifySurface(0.0F, 0.0F, false, false, 0.5F) ==
                Physics::SurfaceOutcome::SurfaceInteraction,
            "missing probability properties must retain surface interaction");
    Require(Physics::ClassifySurface(0.0F, 0.4F, false, true, 0.2F) ==
                Physics::SurfaceOutcome::DirectTransmit,
            "transmittance-only surfaces must transmit below T");
    Require(Physics::EvaluateAbsorption(0.3F, true, 0.2F) ==
                Physics::SurfaceOutcome::Detect,
            "efficiency must detect only on absorption");
    Require(Physics::EvaluateAbsorption(0.3F, true, 0.5F) ==
                Physics::SurfaceOutcome::Absorb,
            "efficiency must leave ordinary absorption");
}

auto TestLambertianDirection() -> void {
    const DeviceVector3 normal{0.0F, 0.0F, 1.0F};
    const auto direction{Physics::SampleLambertian(normal, 0.75F, 0.25F)};

    Require(Physics::Dot(direction, normal) >= 0.0F,
            "Lambertian reflection must remain in the incident medium");
    RequireNear(Physics::Dot(direction, direction), 1.0F, 1.0e-6F,
                "Lambertian direction must be normalized");
}

auto AddSurface(Scene& scene, const char* name) -> std::uint32_t {
    Surface surface{};
    surface.fName = name;
    surface.fType = SurfaceType::DielectricDielectric;
    surface.fModel = SurfaceModel::Unified;
    surface.fFinish = SurfaceFinish::Polished;
    return scene.AddSurface(std::move(surface));
}

auto AddVolume(Scene& scene,
               std::uint32_t volumeID,
               std::uint32_t parentVolumeID,
               std::uint32_t skinSurfaceID) -> void {
    Volume volume{};
    volume.fVolumeID = volumeID;
    volume.fGeometryID = InvalidID;
    volume.fParentVolumeID = parentVolumeID;
    volume.fSkinSurfaceID = skinSurfaceID;
    scene.AddVolume(std::move(volume));
}

auto TestSkinSurfacePriority() -> void {
    Scene scene{};
    const auto parentSkin{AddSurface(scene, "parent-skin")};
    const auto daughterSkin{AddSurface(scene, "daughter-skin")};
    const auto siblingSkin{AddSurface(scene, "sibling-skin")};
    const auto border{AddSurface(scene, "border")};
    AddVolume(scene, 0, InvalidID, parentSkin);
    AddVolume(scene, 1, 0, daughterSkin);
    AddVolume(scene, 2, 0, siblingSkin);

    Require(scene.FindBoundarySurface(0, 1) == scene.FindSurface(daughterSkin),
            "entering a daughter must prefer the daughter skin surface");
    Require(scene.FindBoundarySurface(1, 0) == scene.FindSurface(daughterSkin),
            "leaving a daughter must prefer the daughter skin surface");
    Require(scene.FindBoundarySurface(1, 2) == scene.FindSurface(daughterSkin),
            "sibling crossing must prefer the source skin surface");

    scene.AddSurfaceBinding({0, 1, border});
    Require(scene.FindBoundarySurface(0, 1) == scene.FindSurface(border),
            "border surface must override skin surfaces");
}

auto TestInstanceLocalGeometryFlags() -> void {
    Scene scene{};
    G4GO::Optical::Geometry geometry{};
    geometry.fMesh.fVerticesMm = {
        {0.0, 0.0, 0.0},
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
    };
    geometry.fMesh.fIndices = {0, 1, 2};
    geometry.fMesh.fTriangleNormals = {
        {0.0, 0.0, 1.0}
    };
    geometry.fMesh.fTriangleFlags = {1U};
    const auto geometryID{scene.AddGeometry(std::move(geometry))};
    for (auto volumeID{std::uint32_t{}}; volumeID < 3U; ++volumeID) {
        Volume volume{};
        volume.fVolumeID = volumeID;
        volume.fGeometryID = geometryID;
        scene.AddVolume(std::move(volume));
    }

    scene.EnsureUniqueGeometry(1);
    Require(scene.Volumes().at(0).fGeometryID == geometryID,
            "unmodified instance must retain shared geometry");
    Require(scene.Volumes().at(2).fGeometryID == geometryID,
            "unmodified instance must retain shared geometry");
    Require(scene.Volumes().at(1).fGeometryID != geometryID,
            "contact instance must receive local geometry");
    const auto localGeometryID{scene.Volumes().at(1).fGeometryID};
    Require(scene.FindGeometry(localGeometryID)->fMesh.fTriangleFlags.at(0) ==
                1U,
            "local geometry must preserve triangle flags");
    scene.FindGeometry(localGeometryID)->fMesh.fTriangleFlags.at(0) = 0U;
    Require(scene.FindGeometry(geometryID)->fMesh.fTriangleFlags.at(0) == 1U,
            "local flag edits must not affect shared geometry");
}

auto TestStatisticsValidFields() -> void {
    using G4GO::Optical::AllStatisticFieldBits;
    using G4GO::Optical::PhotonTransportStatisticField;
    using G4GO::Optical::StatisticFieldBit;
    Require(StatisticFieldBit(PhotonTransportStatisticField::Generated) != 0U,
            "generated statistic bit must be defined");
    Require(StatisticFieldBit(PhotonTransportStatisticField::TransportTime) !=
                0U,
            "transport-time statistic bit must be defined");
    Require((AllStatisticFieldBits() &
             StatisticFieldBit(PhotonTransportStatisticField::Detected)) != 0U,
            "all-statistics mask must include detected count");
}

} // namespace

auto main() -> int {
    try {
        TestNormalIncidence();
        TestBrewsterAngle();
        TestTotalInternalReflection();
        TestSurfaceProbabilities();
        TestLambertianDirection();
        TestSkinSurfacePriority();
        TestInstanceLocalGeometryFlags();
        TestStatisticsValidFields();
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
