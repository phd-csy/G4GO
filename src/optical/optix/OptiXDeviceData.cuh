#pragma once

#include <array>
#include <cstdint>

namespace G4GO::Optical {

using DeviceVector3 = std::array<float, 3>;

struct DevicePhoton {
    DeviceVector3 positionMm{};
    float timeNs{};

    DeviceVector3 direction{};
    float energyEv{};

    DeviceVector3 polarization{};

    std::uint32_t eventID{};
    std::uint32_t photonID{};
    std::uint32_t volumeID{};
    std::uint32_t eventIndex{};
};

struct DevicePhotonHit {
    DeviceVector3 positionMm{};
    float timeNs{};

    DeviceVector3 direction{};
    float energyEv{};

    std::uint32_t eventID{};
    std::uint32_t photonID{};
    std::uint32_t sensorID{};
};

struct alignas(16) DeviceOpticalEmission {
    DeviceVector3 positionMm{};
    float timeNs{};

    DeviceVector3 direction{};
    float stepLengthMm{};

    DeviceVector3 stepDeltaMm{};
    float preVelocityMmPerNs{};

    DeviceVector3 polarization{};
    float deltaVelocityMmPerNs{};

    float energyEv{};
    float preMeanPhotonCount{};
    float postMeanPhotonCount{};

    float decayTimeNs{};
    float riseTimeNs{};
    float charge{};
    std::uint32_t eventID{};

    std::uint32_t firstPhotonID{};
    std::uint32_t photonCount{};
    std::uint32_t volumeID{};

    std::uint32_t materialID{};
    std::uint32_t spectrumID{};
    std::uint8_t type{};
};

struct DeviceProperty {
    const float* energyEv{};
    const float* values{};
    std::uint32_t count{};
    std::uint32_t constant{};
};

struct DeviceMaterial {
    DeviceProperty rindex{};
    float rindexMax{1.0F};
    DeviceProperty groupVelocityMmPerNs{};
    DeviceProperty absLengthMm{};
    std::array<DeviceProperty, 3> scintillationSpectrum{};
};

struct DeviceSurface {
    std::uint8_t type{};
    std::uint8_t model{};
    std::uint8_t finish{};
    std::uint8_t reserved{};
    float modelValue{1.0F};
    DeviceProperty reflectivity{};
    DeviceProperty efficiency{};
    DeviceProperty transmittance{};
    DeviceProperty rindex{};
    DeviceProperty specularLobe{};
    DeviceProperty specularSpike{};
    DeviceProperty backscatter{};
    DeviceProperty surfaceRoughness{};
};

struct DeviceMeshGeometry {
    const DeviceVector3* vertices{};
    const std::uint32_t* indices{};
    const DeviceVector3* normals{};
    const std::uint8_t* triangleFlags{};
    std::uint32_t vertexCount{};
    std::uint32_t triangleCount{};
};

struct DeviceGeometry {
    DeviceMeshGeometry mesh{};
};

struct DeviceVolume {
    std::uint32_t volumeID{};
    std::uint32_t physicalVolumeID{};
    std::uint32_t copyNo{};
    std::uint32_t geometryID{};
    std::uint32_t materialID{};
    std::uint32_t parentVolumeID{};
    std::uint32_t skinSurfaceID{};
    std::uint32_t sensorID{};
    std::uint32_t depth{};
    std::uint8_t mayHaveCoincidentBoundary{};
    std::uint8_t reserved[3]{};
};

struct DeviceSurfaceBinding {
    std::uint32_t fromVolumeID{};
    std::uint32_t toVolumeID{};
    std::uint32_t surfaceID{};
};

struct DeviceScene {
    const DeviceMaterial* materials{};
    std::uint32_t materialCount{};
    const DeviceSurface* surfaces{};
    std::uint32_t surfaceCount{};
    const DeviceGeometry* geometries{};
    std::uint32_t geometryCount{};
    const DeviceVolume* volumes{};
    std::uint32_t volumeCount{};
    const DeviceSurfaceBinding* surfaceBindings{};
    std::uint32_t surfaceBindingCount{};
    std::uint32_t worldVolumeID{};
};

struct DeviceTransportStats {
    unsigned long long detectedCount{};
    unsigned long long absorbedCount{};
    unsigned long long escapedCount{};
    unsigned long long truncatedCount{};
    unsigned long long maxBounceCount{};
    unsigned long long invalidStateCount{};
    unsigned long long zeroStepCount{};
    unsigned long long totalBounceCount{};
    unsigned long long coincidentCandidateTraceCount{};
    unsigned long long coincidentCandidateHitCount{};
};

struct DeviceEventTransportStats {
    unsigned long long detectedCount{};
    unsigned long long absorbedCount{};
    unsigned long long escapedCount{};
    unsigned long long truncatedCount{};
    unsigned long long maxBounceCount{};
    unsigned long long invalidStateCount{};
    unsigned long long zeroStepCount{};
};

struct OptixLaunchParams {
    DeviceOpticalEmission* emissions{};
    const std::uint32_t* emissionEventIndices{};
    const std::uint32_t* emissionOffsets{};
    DevicePhotonHit* hits{};
    std::uint32_t* hitFlags{};
    DeviceTransportStats* stats{};
    DeviceEventTransportStats* eventStats{};
    DeviceScene scene{};
    std::uint64_t traversable{};
    std::uint64_t seed{};
    std::uint32_t maxBounceCount{};
    std::uint32_t emissionCount{};
    std::uint32_t eventCount{};
    std::uint32_t photonCount{};
    std::uint32_t enablePerformanceDiagnostics{};
    float boundaryEpsilonMm{};
};

static_assert(sizeof(DevicePhoton) == 60);
static_assert(sizeof(DevicePhotonHit) == 44);
static_assert(sizeof(DeviceOpticalEmission) == 128);

} // namespace G4GO::Optical
