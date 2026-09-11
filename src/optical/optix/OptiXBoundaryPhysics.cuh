#pragma once

#include "OptiXDeviceData.cuh"

#include <cmath>

#ifdef __CUDACC__
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

struct FresnelOutput {
    DeviceVector3 reflectedDirection{};
    DeviceVector3 transmittedDirection{};
    DeviceVector3 reflectedPolarization{};
    DeviceVector3 transmittedPolarization{};
    float transmittance{};
    bool totalInternalReflection{};
};

G4GO_OPTICAL_HD inline auto Add(DeviceVector3 left, DeviceVector3 right) -> DeviceVector3 {
    return {left.at(0) + right.at(0), left.at(1) + right.at(1), left.at(2) + right.at(2)};
}

G4GO_OPTICAL_HD inline auto Subtract(DeviceVector3 left, DeviceVector3 right) -> DeviceVector3 {
    return {left.at(0) - right.at(0), left.at(1) - right.at(1), left.at(2) - right.at(2)};
}

G4GO_OPTICAL_HD inline auto Scale(DeviceVector3 vector, float value) -> DeviceVector3 {
    return {vector.at(0) * value, vector.at(1) * value, vector.at(2) * value};
}

G4GO_OPTICAL_HD inline auto Dot(DeviceVector3 left, DeviceVector3 right) -> float {
    return left.at(0) * right.at(0) + left.at(1) * right.at(1) + left.at(2) * right.at(2);
}

G4GO_OPTICAL_HD inline auto Cross(DeviceVector3 left, DeviceVector3 right) -> DeviceVector3 {
    return {
        left.at(1) * right.at(2) - left.at(2) * right.at(1),
        left.at(2) * right.at(0) - left.at(0) * right.at(2),
        left.at(0) * right.at(1) - left.at(1) * right.at(0),
    };
}

G4GO_OPTICAL_HD inline auto Normalize(DeviceVector3 vector) -> DeviceVector3 {
    const auto lengthSquared{Dot(vector, vector)};
    if (not(lengthSquared > 0.0F)) {
        return {};
    }
    return Scale(vector, 1.0F / sqrtf(lengthSquared));
}

G4GO_OPTICAL_HD inline auto ClampProbability(float value) -> float { return fminf(fmaxf(value, 0.0F), 1.0F); }

G4GO_OPTICAL_HD inline auto ProjectPolarization(DeviceVector3 polarization, DeviceVector3 direction) -> DeviceVector3 {
    auto output{Subtract(polarization, Scale(direction, Dot(polarization, direction)))};
    if (Dot(output, output) < 1.0e-12F) {
        const auto helper{
            fabsf(direction.at(2)) < 0.9F ? DeviceVector3{0.0F, 0.0F, 1.0F}
                : DeviceVector3{1.0F, 0.0F, 0.0F}
        };
        output = Cross(direction, helper);
    }
    return Normalize(output);
}

G4GO_OPTICAL_HD inline auto ReflectDirection(DeviceVector3 direction, DeviceVector3 normal) -> DeviceVector3 {
    return Normalize(Subtract(direction, Scale(normal, 2.0F * Dot(direction, normal))));
}

G4GO_OPTICAL_HD inline auto ReflectPolarization(DeviceVector3 polarization, DeviceVector3 normal,
                                                DeviceVector3 direction) -> DeviceVector3 {
    const auto reflected{Add(Scale(polarization, -1.0F), Scale(normal, 2.0F * Dot(polarization, normal)))};
    return ProjectPolarization(reflected, direction);
}

G4GO_OPTICAL_HD inline auto ClassifySurface(float reflectivity, float transmittance, bool hasReflectivity,
                                            bool hasTransmittance, float sample) -> SurfaceOutcome {
    const auto normalizedSample{ClampProbability(sample)};
    const auto reflectionThreshold{hasReflectivity ? ClampProbability(reflectivity) : 0.0F};
    const auto transmissionThreshold{hasTransmittance ? ClampProbability(transmittance) : 0.0F};
    if (not hasReflectivity and not hasTransmittance) {
        return SurfaceOutcome::SurfaceInteraction;
    }
    if (normalizedSample > reflectionThreshold + transmissionThreshold) {
        return SurfaceOutcome::Absorb;
    }
    if (normalizedSample > reflectionThreshold) {
        return SurfaceOutcome::DirectTransmit;
    }
    return SurfaceOutcome::SurfaceInteraction;
}

G4GO_OPTICAL_HD inline auto EvaluateAbsorption(float efficiency, bool sensor, float sample) -> SurfaceOutcome {
    if (sensor and sample < ClampProbability(efficiency)) {
        return SurfaceOutcome::Detect;
    }
    return SurfaceOutcome::Absorb;
}

G4GO_OPTICAL_HD inline auto SampleLambertian(DeviceVector3 normal, float radialSample,
                                             float azimuthSample) -> DeviceVector3 {
    constexpr auto twoPi{6.28318530717958647692F};
    normal = Normalize(normal);
    radialSample = ClampProbability(radialSample);
    azimuthSample = ClampProbability(azimuthSample);
    const auto radial{sqrtf(radialSample)};
    const auto azimuth{twoPi * azimuthSample};
    const auto normalComponent{sqrtf(fmaxf(0.0F, 1.0F - radialSample))};
    const auto helper{
        fabsf(normal.at(2)) < 0.9F ? DeviceVector3{0.0F, 0.0F, 1.0F}
            : DeviceVector3{1.0F, 0.0F, 0.0F}
    };
    const auto tangent{Normalize(Cross(helper, normal))};
    const auto bitangent{Cross(normal, tangent)};
    return Normalize(Add(Add(Scale(tangent, radial * cosf(azimuth)), Scale(bitangent, radial * sinf(azimuth))),
                         Scale(normal, normalComponent)));
}

G4GO_OPTICAL_HD inline auto ComputeFresnel(DeviceVector3 direction, DeviceVector3 polarization, DeviceVector3 normal,
                                           float indexFrom, float indexTo) -> FresnelOutput {
    direction = Normalize(direction);
    normal = Normalize(normal);
    polarization = ProjectPolarization(polarization, direction);

    const auto cosineIncident{ClampProbability(-Dot(direction, normal))};
    const auto eta{indexFrom / indexTo};
    const auto sineTransmittedSquared{eta * eta * (1.0F - cosineIncident * cosineIncident)};

    FresnelOutput output{};
    output.reflectedDirection = ReflectDirection(direction, normal);
    output.totalInternalReflection = sineTransmittedSquared >= 1.0F;
    if (output.totalInternalReflection) {
        output.reflectedPolarization = ReflectPolarization(polarization, normal, output.reflectedDirection);
        return output;
    }

    const auto cosineTransmitted{sqrtf(fmaxf(0.0F, 1.0F - sineTransmittedSquared))};
    output.transmittedDirection =
        Normalize(Add(Scale(direction, eta), Scale(normal, eta * cosineIncident - cosineTransmitted)));

    const auto transverse{Cross(direction, normal)};
    const auto transverseLengthSquared{Dot(transverse, transverse)};
    if (transverseLengthSquared < 1.0e-12F) {
        const auto denominator{indexFrom + indexTo};
        const auto reflectionAmplitude{(indexFrom - indexTo) / denominator};
        output.transmittance = ClampProbability(1.0F - reflectionAmplitude * reflectionAmplitude);
        output.reflectedPolarization = indexTo > indexFrom ? Scale(polarization, -1.0F) : polarization;
        output.transmittedPolarization = polarization;
        return output;
    }

    const auto sDirection{Normalize(transverse)};
    const auto incidentPDirection{Normalize(Cross(direction, sDirection))};
    const auto incidentS{Dot(polarization, sDirection)};
    const auto incidentP{Dot(polarization, incidentPDirection)};

    const auto sDenominator{indexFrom * cosineIncident + indexTo * cosineTransmitted};
    const auto pDenominator{indexTo * cosineIncident + indexFrom * cosineTransmitted};
    const auto reflectedS{(indexFrom * cosineIncident - indexTo * cosineTransmitted) / sDenominator};
    const auto reflectedP{(indexTo * cosineIncident - indexFrom * cosineTransmitted) / pDenominator};
    const auto transmittedS{2.0F * indexFrom * cosineIncident / sDenominator};
    const auto transmittedP{2.0F * indexFrom * cosineIncident / pDenominator};

    const auto reflectionProbability{incidentS * incidentS * reflectedS * reflectedS +
                                     incidentP * incidentP * reflectedP * reflectedP};
    output.transmittance = ClampProbability(1.0F - reflectionProbability);

    const auto reflectedPDirection{Normalize(Cross(output.reflectedDirection, sDirection))};
    const auto transmittedPDirection{Normalize(Cross(output.transmittedDirection, sDirection))};
    output.reflectedPolarization =
        Normalize(Add(Scale(sDirection, incidentS * reflectedS), Scale(reflectedPDirection, incidentP * reflectedP)));
    output.transmittedPolarization = Normalize(
        Add(Scale(sDirection, incidentS * transmittedS), Scale(transmittedPDirection, incidentP * transmittedP)));
    return output;
}

} // namespace G4GO::Optical::BoundaryPhysics

#undef G4GO_OPTICAL_HD
