#pragma once

#include <cstdint>

namespace G4GO::Optical {

struct DeviceVector3 {
    float fX{};
    float fY{};
    float fZ{};
};

struct DevicePhoton {
    DeviceVector3 fPositionMm{};
    float fTimeNs{};

    DeviceVector3 fDirection{};
    float fEnergyEv{};

    DeviceVector3 fPolarization{};
    float fWeight{1.0F};

    std::uint64_t fPhotonID{};
    std::uint32_t fVolumeID{};
    std::uint8_t fSource{};
    std::uint8_t fFlags{};
    std::uint16_t fReserved{};
};

struct DevicePhotonHit {
    DeviceVector3 fPositionMm{};
    float fTimeNs{};

    DeviceVector3 fDirection{};
    float fEnergyEv{};

    std::uint64_t fPhotonID{};
    std::uint32_t fSensorID{};
    std::uint32_t fFlags{};
};

struct DeviceProperty {
    const float* fEnergyEv{};
    const float* fValues{};
    std::uint32_t fCount{};
};

struct DeviceMaterial {
    DeviceProperty fRindex{};
    DeviceProperty fGroupVelocityMmPerNs{};
    DeviceProperty fAbsLengthMm{};
};

struct DeviceSurface {
    std::uint8_t fKind{};
    std::uint8_t fFinish{};
    std::uint8_t fSensor{};
    std::uint8_t fReserved{};
    DeviceProperty fReflectivity{};
    DeviceProperty fEfficiency{};
};

struct DeviceRotation {
    float fXX{1.0F};
    float fXY{};
    float fXZ{};
    float fYX{};
    float fYY{1.0F};
    float fYZ{};
    float fZX{};
    float fZY{};
    float fZZ{1.0F};
};

struct DeviceSolid {
    std::uint8_t fKind{};
    std::uint8_t fReserved[3]{};
    DeviceRotation fRotation{};
    DeviceVector3 fTranslationMm{};
    DeviceVector3 fHalfSizeMm{};
    float fInnerRadiusMm{};
    float fOuterRadiusMm{};
    float fHalfLengthMm{};
    float fStartPhi{};
    float fDeltaPhi{};
    std::uint32_t fVolumeID{};
    std::uint32_t fMaterialID{};
    std::uint32_t fSkinSurfaceID{};
    std::uint32_t fSensorID{};
    std::uint32_t fDepth{};
};

struct DeviceSurfaceBinding {
    std::uint32_t fFromVolumeID{};
    std::uint32_t fToVolumeID{};
    std::uint32_t fSurfaceID{};
};

struct DeviceScene {
    const DeviceMaterial* fMaterials{};
    std::uint32_t fMaterialCount{};
    const DeviceSurface* fSurfaces{};
    std::uint32_t fSurfaceCount{};
    const DeviceSolid* fSolids{};
    std::uint32_t fSolidCount{};
    const DeviceSurfaceBinding* fSurfaceBindings{};
    std::uint32_t fSurfaceBindingCount{};
    std::uint32_t fWorldVolumeID{};
};

struct DeviceTransportStats {
    unsigned long long fDetectedCount{};
    unsigned long long fAbsorbedCount{};
    unsigned long long fEscapedCount{};
    unsigned long long fTruncatedCount{};
    unsigned long long fMaxBounceCount{};
    unsigned long long fInvalidStateCount{};
    unsigned long long fZeroStepCount{};
};

struct OptixLaunchParams {
    DevicePhoton* fPhotons{};
    DevicePhotonHit* fHits{};
    std::uint32_t* fHitFlags{};
    DeviceTransportStats* fStats{};
    DeviceScene fScene{};
    std::uint64_t fTraversable{};
    std::uint64_t fSeed{};
    std::uint32_t fMaxBounceCount{};
    std::uint32_t fPhotonCount{};
    float fBoundaryEpsilonMm{};
};

static_assert(sizeof(DevicePhoton) == 64);
static_assert(sizeof(DevicePhotonHit) == 48);

} // namespace G4GO::Optical
