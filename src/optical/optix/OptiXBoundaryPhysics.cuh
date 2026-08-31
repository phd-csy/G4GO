#pragma once

#include "OptiXDeviceData.cuh"

#include <cmath>

#if defined(__CUDACC__)
#    define G4GO_OPTICAL_HD __host__ __device__
#else
#    define G4GO_OPTICAL_HD
#endif

namespace G4GO::Optical::BoundaryPhysics {

enum class SurfaceOutcome : unsigned char {
    Absorb,
    Detect,
    DirectTransmit,
    SurfaceInteraction,
};

struct FresnelResult {
    DeviceVector3 fReflectedDirection{};
    DeviceVector3 fTransmittedDirection{};
    DeviceVector3 fReflectedPolarization{};
    DeviceVector3 fTransmittedPolarization{};
    float fTransmittance{};
    bool fTotalInternalReflection{};
};

G4GO_OPTICAL_HD inline auto Add(DeviceVector3 left, DeviceVector3 right)
    -> DeviceVector3 {
    return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}

G4GO_OPTICAL_HD inline auto Subtract(DeviceVector3 left, DeviceVector3 right)
    -> DeviceVector3 {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

G4GO_OPTICAL_HD inline auto Scale(DeviceVector3 vector, float value)
    -> DeviceVector3 {
    return {vector[0] * value, vector[1] * value, vector[2] * value};
}

G4GO_OPTICAL_HD inline auto Dot(DeviceVector3 left, DeviceVector3 right)
    -> float {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

G4GO_OPTICAL_HD inline auto Cross(DeviceVector3 left, DeviceVector3 right)
    -> DeviceVector3 {
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    };
}

G4GO_OPTICAL_HD inline auto Normalize(DeviceVector3 vector) -> DeviceVector3 {
    const auto lengthSquared{Dot(vector, vector)};
    if (!(lengthSquared > 0.0F)) {
        return {};
    }
    return Scale(vector, 1.0F / sqrtf(lengthSquared));
}

G4GO_OPTICAL_HD inline auto ClampProbability(float value) -> float {
    return fminf(fmaxf(value, 0.0F), 1.0F);
}

G4GO_OPTICAL_HD inline auto ProjectPolarization(DeviceVector3 polarization,
                                                DeviceVector3 direction)
    -> DeviceVector3 {
    auto result{Subtract(polarization,
                         Scale(direction, Dot(polarization, direction)))};
    if (Dot(result, result) < 1.0e-12F) {
        const auto helper{
            fabsf(direction[2]) < 0.9F ? DeviceVector3{0.0F, 0.0F, 1.0F}
                : DeviceVector3{1.0F, 0.0F, 0.0F}
        };
        result = Cross(direction, helper);
    }
    return Normalize(result);
}

G4GO_OPTICAL_HD inline auto ReflectDirection(DeviceVector3 direction,
                                             DeviceVector3 normal)
    -> DeviceVector3 {
    return Normalize(Subtract(
        direction, Scale(normal, 2.0F * Dot(direction, normal))));
}

G4GO_OPTICAL_HD inline auto ReflectPolarization(DeviceVector3 polarization,
                                                DeviceVector3 normal,
                                                DeviceVector3 direction)
    -> DeviceVector3 {
    const auto reflected{Add(Scale(polarization, -1.0F),
                             Scale(normal,
                                   2.0F * Dot(polarization, normal)))};
    return ProjectPolarization(reflected, direction);
}

G4GO_OPTICAL_HD inline auto ClassifySurface(float reflectivity,
                                            float transmittance,
                                            bool hasReflectivity,
                                            bool hasTransmittance,
                                            float sample) -> SurfaceOutcome {
    const auto directTransmission{ClampProbability(transmittance)};
    const auto surfaceInteraction{
        hasReflectivity ? (1.0F - directTransmission) * ClampProbability(reflectivity) : (hasTransmittance ? 0.0F : 1.0F)};
    if (sample < directTransmission) {
        return SurfaceOutcome::DirectTransmit;
    }
    if (sample < directTransmission + surfaceInteraction) {
        return SurfaceOutcome::SurfaceInteraction;
    }
    return SurfaceOutcome::Absorb;
}

G4GO_OPTICAL_HD inline auto EvaluateAbsorption(float efficiency,
                                               bool sensor,
                                               float sample) -> SurfaceOutcome {
    if (sensor && sample < ClampProbability(efficiency)) {
        return SurfaceOutcome::Detect;
    }
    return SurfaceOutcome::Absorb;
}

G4GO_OPTICAL_HD inline auto SampleLambertian(DeviceVector3 normal,
                                             float radialSample,
                                             float azimuthSample)
    -> DeviceVector3 {
    constexpr auto twoPi{6.28318530717958647692F};
    normal = Normalize(normal);
    radialSample = ClampProbability(radialSample);
    azimuthSample = ClampProbability(azimuthSample);
    const auto radial{sqrtf(radialSample)};
    const auto azimuth{twoPi * azimuthSample};
    const auto normalComponent{
        sqrtf(fmaxf(0.0F, 1.0F - radialSample))};
    const auto helper{
        fabsf(normal[2]) < 0.9F ? DeviceVector3{0.0F, 0.0F, 1.0F}
            : DeviceVector3{1.0F, 0.0F, 0.0F}
    };
    const auto tangent{Normalize(Cross(helper, normal))};
    const auto bitangent{Cross(normal, tangent)};
    return Normalize(Add(
        Add(Scale(tangent, radial * cosf(azimuth)),
            Scale(bitangent, radial * sinf(azimuth))),
        Scale(normal, normalComponent)));
}

G4GO_OPTICAL_HD inline auto ComputeFresnel(DeviceVector3 direction,
                                           DeviceVector3 polarization,
                                           DeviceVector3 normal,
                                           float indexFrom,
                                           float indexTo) -> FresnelResult {
    direction = Normalize(direction);
    normal = Normalize(normal);
    polarization = ProjectPolarization(polarization, direction);

    const auto cosineIncident{
        ClampProbability(-Dot(direction, normal))};
    const auto eta{indexFrom / indexTo};
    const auto sineTransmittedSquared{
        eta * eta * (1.0F - cosineIncident * cosineIncident)};

    FresnelResult result{};
    result.fReflectedDirection = ReflectDirection(direction, normal);
    result.fTotalInternalReflection = sineTransmittedSquared >= 1.0F;
    if (result.fTotalInternalReflection) {
        result.fReflectedPolarization = ReflectPolarization(
            polarization, normal, result.fReflectedDirection);
        return result;
    }

    const auto cosineTransmitted{
        sqrtf(fmaxf(0.0F, 1.0F - sineTransmittedSquared))};
    result.fTransmittedDirection = Normalize(Add(
        Scale(direction, eta),
        Scale(normal, eta * cosineIncident - cosineTransmitted)));

    const auto transverse{Cross(direction, normal)};
    const auto transverseLengthSquared{Dot(transverse, transverse)};
    if (transverseLengthSquared < 1.0e-12F) {
        const auto denominator{indexFrom + indexTo};
        const auto reflectionAmplitude{(indexFrom - indexTo) / denominator};
        result.fTransmittance = ClampProbability(
            1.0F - reflectionAmplitude * reflectionAmplitude);
        result.fReflectedPolarization =
            indexTo > indexFrom ? Scale(polarization, -1.0F) : polarization;
        result.fTransmittedPolarization = polarization;
        return result;
    }

    const auto sDirection{Normalize(transverse)};
    const auto incidentPDirection{Normalize(Cross(direction, sDirection))};
    const auto incidentS{Dot(polarization, sDirection)};
    const auto incidentP{Dot(polarization, incidentPDirection)};

    const auto sDenominator{indexFrom * cosineIncident +
                            indexTo * cosineTransmitted};
    const auto pDenominator{indexTo * cosineIncident +
                            indexFrom * cosineTransmitted};
    const auto reflectedS{(indexFrom * cosineIncident -
                           indexTo * cosineTransmitted) /
                          sDenominator};
    const auto reflectedP{(indexTo * cosineIncident -
                           indexFrom * cosineTransmitted) /
                          pDenominator};
    const auto transmittedS{2.0F * indexFrom * cosineIncident / sDenominator};
    const auto transmittedP{2.0F * indexFrom * cosineIncident / pDenominator};

    const auto reflectionProbability{
        incidentS * incidentS * reflectedS * reflectedS +
        incidentP * incidentP * reflectedP * reflectedP};
    result.fTransmittance = ClampProbability(1.0F - reflectionProbability);

    const auto reflectedPDirection{
        Normalize(Cross(result.fReflectedDirection, sDirection))};
    const auto transmittedPDirection{
        Normalize(Cross(result.fTransmittedDirection, sDirection))};
    result.fReflectedPolarization = Normalize(Add(
        Scale(sDirection, incidentS * reflectedS),
        Scale(reflectedPDirection, incidentP * reflectedP)));
    result.fTransmittedPolarization = Normalize(Add(
        Scale(sDirection, incidentS * transmittedS),
        Scale(transmittedPDirection, incidentP * transmittedP)));
    return result;
}

} // namespace G4GO::Optical::BoundaryPhysics

#undef G4GO_OPTICAL_HD
