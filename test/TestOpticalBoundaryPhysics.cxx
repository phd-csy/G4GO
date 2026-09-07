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

auto main() -> int {
    using G4GO::Optical::DeviceVector3;
    using G4GO::Optical::InvalidID;
    using G4GO::Optical::Scene;
    using G4GO::Optical::Surface;
    using G4GO::Optical::SurfaceFinish;
    using G4GO::Optical::SurfaceModel;
    using G4GO::Optical::SurfaceType;
    using G4GO::Optical::Volume;
    namespace Physics = G4GO::Optical::BoundaryPhysics;

    const auto require{[](bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }};
    const auto requireNear{[&require](float actual, float expected, float tolerance, const std::string& message) {
        require(std::abs(actual - expected) <= tolerance,
                message + ": actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected));
    }};

    try {
        {
            const auto output{
                Physics::ComputeFresnel({0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, 1.0F, 1.5F)};

            require(!output.totalInternalReflection, "normal incidence must transmit");
            requireNear(output.transmittance, 0.96F, 1.0e-5F, "normal-incidence transmittance");
            requireNear(output.reflectedDirection.at(2), -1.0F, 1.0e-6F, "normal-incidence reflected direction");
            requireNear(output.transmittedDirection.at(2), 1.0F, 1.0e-6F, "normal-incidence transmitted direction");
            requireNear(Physics::Dot(output.reflectedDirection, output.reflectedPolarization), 0.0F, 1.0e-6F,
                        "reflected polarization must be transverse");
        }

        {
            const auto angle{std::atan(1.5F)};
            const DeviceVector3 direction{std::sin(angle), 0.0F, std::cos(angle)};
            const DeviceVector3 sDirection{0.0F, 1.0F, 0.0F};
            const auto pPolarization{Physics::Normalize(Physics::Cross(direction, sDirection))};
            const auto output{Physics::ComputeFresnel(direction, pPolarization, {0.0F, 0.0F, -1.0F}, 1.0F, 1.5F)};

            requireNear(output.transmittance, 1.0F, 1.0e-5F, "P-polarized Brewster transmittance");
        }

        {
            constexpr auto angle{50.0F * std::numbers::pi_v<float> / 180.0F};
            const auto output{Physics::ComputeFresnel({std::sin(angle), 0.0F, std::cos(angle)}, {0.0F, 1.0F, 0.0F},
                                                      {0.0F, 0.0F, -1.0F}, 1.5F, 1.0F)};

            require(output.totalInternalReflection, "50 degree glass-to-air incidence must be TIR");
            requireNear(output.transmittance, 0.0F, 1.0e-6F, "TIR transmittance");
            require(output.reflectedDirection.at(2) < 0.0F, "TIR direction must return to the incident medium");
            requireNear(Physics::Dot(output.reflectedDirection, output.reflectedPolarization), 0.0F, 1.0e-6F,
                        "TIR polarization must be transverse");
        }

        {
            require(Physics::ClassifySurface(0.8F, 0.1F, true, true, 0.05F) ==
                        Physics::SurfaceOutcome::SurfaceInteraction,
                    "reflectivity must select surface interaction below R");
            require(Physics::ClassifySurface(0.8F, 0.1F, true, true, 0.75F) ==
                        Physics::SurfaceOutcome::SurfaceInteraction,
                    "reflectivity must include the upper R threshold");
            require(Physics::ClassifySurface(0.8F, 0.1F, true, true, 0.85F) == Physics::SurfaceOutcome::DirectTransmit,
                    "transmittance must occupy the interval after R");
            require(Physics::ClassifySurface(0.8F, 0.1F, true, true, 0.95F) == Physics::SurfaceOutcome::Absorb,
                    "remaining surface probability must absorb");
            require(Physics::ClassifySurface(0.0F, 0.0F, false, false, 0.5F) ==
                        Physics::SurfaceOutcome::SurfaceInteraction,
                    "missing probability properties must retain surface "
                    "interaction");
            require(Physics::ClassifySurface(0.0F, 0.4F, false, true, 0.2F) == Physics::SurfaceOutcome::DirectTransmit,
                    "transmittance-only surfaces must transmit below T");
            require(Physics::EvaluateAbsorption(0.3F, true, 0.2F) == Physics::SurfaceOutcome::Detect,
                    "efficiency must detect only on absorption");
            require(Physics::EvaluateAbsorption(0.3F, true, 0.5F) == Physics::SurfaceOutcome::Absorb,
                    "efficiency must leave ordinary absorption");
        }

        {
            const DeviceVector3 normal{0.0F, 0.0F, 1.0F};
            const auto direction{Physics::SampleLambertian(normal, 0.75F, 0.25F)};

            require(Physics::Dot(direction, normal) >= 0.0F, "Lambertian reflection must remain in the incident "
                                                             "medium");
            requireNear(Physics::Dot(direction, direction), 1.0F, 1.0e-6F, "Lambertian direction must be normalized");
        }

        {
            Scene scene{};
            Surface parentSkin{};
            parentSkin.name = "parent-skin";
            parentSkin.type = SurfaceType::DielectricDielectric;
            parentSkin.model = SurfaceModel::Unified;
            parentSkin.finish = SurfaceFinish::Polished;
            const auto parentSkinID{scene.AddSurface(std::move(parentSkin))};

            Surface daughterSkin{};
            daughterSkin.name = "daughter-skin";
            daughterSkin.type = SurfaceType::DielectricDielectric;
            daughterSkin.model = SurfaceModel::Unified;
            daughterSkin.finish = SurfaceFinish::Polished;
            const auto daughterSkinID{scene.AddSurface(std::move(daughterSkin))};

            Surface siblingSkin{};
            siblingSkin.name = "sibling-skin";
            siblingSkin.type = SurfaceType::DielectricDielectric;
            siblingSkin.model = SurfaceModel::Unified;
            siblingSkin.finish = SurfaceFinish::Polished;
            const auto siblingSkinID{scene.AddSurface(std::move(siblingSkin))};

            Surface border{};
            border.name = "border";
            border.type = SurfaceType::DielectricDielectric;
            border.model = SurfaceModel::Unified;
            border.finish = SurfaceFinish::Polished;
            const auto borderID{scene.AddSurface(std::move(border))};

            Volume parent{};
            parent.volumeID = 0;
            parent.geometryID = InvalidID;
            parent.parentVolumeID = InvalidID;
            parent.skinSurfaceID = parentSkinID;
            scene.AddVolume(std::move(parent));

            Volume daughter{};
            daughter.volumeID = 1;
            daughter.geometryID = InvalidID;
            daughter.parentVolumeID = 0;
            daughter.skinSurfaceID = daughterSkinID;
            scene.AddVolume(std::move(daughter));

            Volume sibling{};
            sibling.volumeID = 2;
            sibling.geometryID = InvalidID;
            sibling.parentVolumeID = 0;
            sibling.skinSurfaceID = siblingSkinID;
            scene.AddVolume(std::move(sibling));

            require(scene.FindBoundarySurface(0, 1) == scene.FindSurface(daughterSkinID),
                    "entering a daughter must prefer the daughter skin "
                    "surface");
            require(scene.FindBoundarySurface(1, 0) == scene.FindSurface(daughterSkinID),
                    "leaving a daughter must prefer the daughter skin "
                    "surface");
            require(scene.FindBoundarySurface(1, 2) == scene.FindSurface(daughterSkinID),
                    "sibling crossing must prefer the source skin surface");

            scene.AddSurfaceBinding({0, 1, borderID});
            require(scene.FindBoundarySurface(0, 1) == scene.FindSurface(borderID),
                    "border surface must override skin surfaces");
        }

        {
            Scene scene{};
            G4GO::Optical::Geometry geometry{};
            geometry.mesh.verticesMm = {
                {0.0, 0.0, 0.0},
                {1.0, 0.0, 0.0},
                {0.0, 1.0, 0.0},
            };
            geometry.mesh.indices = {0, 1, 2};
            geometry.mesh.triangleNormals = {
                {0.0, 0.0, 1.0},
            };
            geometry.mesh.triangleFlags = {1U};
            const auto geometryID{scene.AddGeometry(std::move(geometry))};
            for (auto volumeID{std::uint32_t{}}; volumeID < 3U; ++volumeID) {
                Volume volume{};
                volume.volumeID = volumeID;
                volume.geometryID = geometryID;
                scene.AddVolume(std::move(volume));
            }

            scene.EnsureUniqueGeometry(1);
            require(scene.Volumes().at(0).geometryID == geometryID, "unmodified instance must retain shared geometry");
            require(scene.Volumes().at(2).geometryID == geometryID, "unmodified instance must retain shared geometry");
            require(scene.Volumes().at(1).geometryID != geometryID, "contact instance must receive local geometry");
            const auto localGeometryID{scene.Volumes().at(1).geometryID};
            require(scene.FindGeometry(localGeometryID)->mesh.triangleFlags.at(0) == 1U,
                    "local geometry must preserve triangle flags");
            scene.FindGeometry(localGeometryID)->mesh.triangleFlags.at(0) = 0U;
            require(scene.FindGeometry(geometryID)->mesh.triangleFlags.at(0) == 1U,
                    "local flag edits must not affect shared geometry");
        }

    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
