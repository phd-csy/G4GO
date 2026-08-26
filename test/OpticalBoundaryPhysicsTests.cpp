#include "BoundaryPhysics.cuh"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {

using G4GO::Optical::DeviceVector3;
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
    const auto result{Physics::ComputeFresnel(
        {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, -1.0F}, 1.0F, 1.5F)};

    Require(!result.fTotalInternalReflection,
            "normal incidence must transmit");
    RequireNear(result.fTransmittance, 0.96F, 1.0e-5F,
                "normal-incidence transmittance");
    RequireNear(result.fReflectedDirection.fZ, -1.0F, 1.0e-6F,
                "normal-incidence reflected direction");
    RequireNear(result.fTransmittedDirection.fZ, 1.0F, 1.0e-6F,
                "normal-incidence transmitted direction");
    RequireNear(Physics::Dot(result.fReflectedDirection,
                             result.fReflectedPolarization),
                0.0F, 1.0e-6F,
                "reflected polarization must be transverse");
}

auto TestBrewsterAngle() -> void {
    const auto angle{std::atan(1.5F)};
    const DeviceVector3 direction{std::sin(angle), 0.0F, std::cos(angle)};
    const DeviceVector3 sDirection{0.0F, 1.0F, 0.0F};
    const auto pPolarization{Physics::Normalize(
        Physics::Cross(direction, sDirection))};
    const auto result{Physics::ComputeFresnel(
        direction, pPolarization, {0.0F, 0.0F, -1.0F}, 1.0F, 1.5F)};

    RequireNear(result.fTransmittance, 1.0F, 1.0e-5F,
                "P-polarized Brewster transmittance");
}

auto TestTotalInternalReflection() -> void {
    constexpr auto angle{50.0F * std::numbers::pi_v<float> / 180.0F};
    const auto result{Physics::ComputeFresnel(
        {std::sin(angle), 0.0F, std::cos(angle)}, {0.0F, 1.0F, 0.0F},
        {0.0F, 0.0F, -1.0F}, 1.5F, 1.0F)};

    Require(result.fTotalInternalReflection,
            "50 degree glass-to-air incidence must be TIR");
    RequireNear(result.fTransmittance, 0.0F, 1.0e-6F,
                "TIR transmittance");
    Require(result.fReflectedDirection.fZ < 0.0F,
            "TIR direction must return to the incident medium");
    RequireNear(Physics::Dot(result.fReflectedDirection,
                             result.fReflectedPolarization),
                0.0F, 1.0e-6F,
                "TIR polarization must be transverse");
}

auto TestSurfaceProbabilities() -> void {
    Require(Physics::EvaluateSurface(0.3F, 0.9F, true, 0.2F) ==
                Physics::SurfaceOutcome::Reflect,
            "sensor surface must reflect below reflectivity");
    Require(Physics::EvaluateSurface(0.3F, 0.9F, true, 0.92F) ==
                Physics::SurfaceOutcome::Detect,
            "sensor surface must detect after reflection");
    Require(Physics::EvaluateSurface(0.3F, 0.9F, true, 0.95F) ==
                Physics::SurfaceOutcome::Absorb,
            "sensor surface must absorb after detection");
    Require(Physics::EvaluateSurface(0.3F, 0.0F, true, 0.2F) ==
                Physics::SurfaceOutcome::Detect,
            "zero-reflectivity sensor must detect below efficiency");
    Require(Physics::EvaluateSurface(0.0F, 0.8F, false, 0.5F) ==
                Physics::SurfaceOutcome::Reflect,
            "ordinary surface must reflect below reflectivity");
    Require(Physics::EvaluateSurface(0.0F, 0.8F, false, 0.9F) ==
                Physics::SurfaceOutcome::Absorb,
            "ordinary surface must absorb above reflectivity");
}

auto TestLambertianDirection() -> void {
    const DeviceVector3 normal{0.0F, 0.0F, 1.0F};
    const auto direction{Physics::SampleLambertian(normal, 0.75F, 0.25F)};

    Require(Physics::Dot(direction, normal) >= 0.0F,
            "Lambertian reflection must remain in the incident medium");
    RequireNear(Physics::Dot(direction, direction), 1.0F, 1.0e-6F,
                "Lambertian direction must be normalized");
}

} // namespace

auto main() -> int {
    try {
        TestNormalIncidence();
        TestBrewsterAngle();
        TestTotalInternalReflection();
        TestSurfaceProbabilities();
        TestLambertianDirection();
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
