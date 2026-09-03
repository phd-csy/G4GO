#pragma once

#include <array>
#include <cstdint>

namespace G4GO::Optical {

using DeviceVector3 = std::array<float, 3>;

struct DevicePhoton {
    DeviceVector3 fPositionMm{};
    float fTimeNs{};

    DeviceVector3 fDirection{};
    float fEnergyEv{};

    DeviceVector3 fPolarization{};
    float fWeight{1.0F};

    std::uint32_t fEventID{};
    std::uint32_t fPhotonID{};
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

    std::uint32_t fEventID{};
    std::uint32_t fPhotonID{};
    std::uint32_t fSensorID{};
    std::uint32_t fFlags{};
};

struct alignas(16) DeviceOpticalEmission {
    DeviceVector3 fPositionMm{};
    float fTimeNs{};

    DeviceVector3 fDirection{};
    float fStepLengthMm{};

    DeviceVector3 fStepDeltaMm{};
    float fPreVelocityMmPerNs{};

    DeviceVector3 fPolarization{};
    float fDeltaVelocityMmPerNs{};

    float fEnergyEv{};
    float fWeight{1.0F};
    float fPreMeanPhotonCount{};
    float fPostMeanPhotonCount{};

    float fDecayTimeNs{};
    float fRiseTimeNs{};
    float fCharge{};
    std::uint32_t fEventID{};

    std::uint32_t fEmissionID{};
    std::uint32_t fFirstPhotonID{};
    std::uint32_t fPhotonCount{};
    std::uint32_t fVolumeID{};

    std::uint32_t fMaterialID{};
    std::uint32_t fSpectrumID{};
    std::uint8_t fType{};
    std::uint8_t fFlags{};
    std::uint16_t fReserved{};
};

struct DeviceProperty {
    const float* fEnergyEv{};
    const float* fValues{};
    std::uint32_t fCount{};
    std::uint32_t fConstant{};
};

struct DeviceMaterial {
    DeviceProperty fRindex{};
    DeviceProperty fGroupVelocityMmPerNs{};
    DeviceProperty fAbsLengthMm{};
    DeviceProperty fScintillationSpectrum[3]{};
};

struct DeviceSurface {
    std::uint8_t fType{};
    std::uint8_t fModel{};
    std::uint8_t fFinish{};
    std::uint8_t fReserved{};
    float fModelValue{1.0F};
    DeviceProperty fReflectivity{};
    DeviceProperty fEfficiency{};
    DeviceProperty fTransmittance{};
    DeviceProperty fRindex{};
    DeviceProperty fRealRindex{};
    DeviceProperty fImaginaryRindex{};
    DeviceProperty fCoatedRindex{};
    DeviceProperty fSpecularLobe{};
    DeviceProperty fSpecularSpike{};
    DeviceProperty fBackscatter{};
    DeviceProperty fSurfaceRoughness{};
    DeviceProperty fDichroic{};
    float fCoatedThicknessMm{};
    std::uint8_t fCoatedFrustratedTransmission{1};
    std::uint8_t fReservedSurface[3]{};
};

struct DeviceMeshGeometry {
    const DeviceVector3* fVertices{};
    const std::uint32_t* fIndices{};
    const DeviceVector3* fNormals{};
    const std::uint8_t* fTriangleFlags{};
    std::uint32_t fVertexCount{};
    std::uint32_t fTriangleCount{};
};

struct DeviceGeometry {
    DeviceMeshGeometry fMesh{};
};

struct DeviceVolume {
    std::uint32_t fVolumeID{};
    std::uint32_t fPhysicalVolumeID{};
    std::uint32_t fCopyNo{};
    std::uint32_t fGeometryID{};
    std::uint32_t fMaterialID{};
    std::uint32_t fParentVolumeID{};
    std::uint32_t fSkinSurfaceID{};
    std::uint32_t fSensorID{};
    std::uint32_t fDepth{};
    std::uint8_t fMayHaveCoincidentBoundary{};
    std::uint8_t fReserved[3]{};
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
    const DeviceGeometry* fGeometries{};
    std::uint32_t fGeometryCount{};
    const DeviceVolume* fVolumes{};
    std::uint32_t fVolumeCount{};
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
    unsigned long long fTotalBounceCount{};
    unsigned long long fCoincidentCandidateTraceCount{};
    unsigned long long fCoincidentCandidateHitCount{};
};

struct OptixLaunchParams {
    DeviceOpticalEmission* fEmissions{};
    const std::uint32_t* fEmissionOffsets{};
    DevicePhotonHit* fHits{};
    std::uint32_t* fHitFlags{};
    DeviceTransportStats* fStats{};
    DeviceScene fScene{};
    std::uint64_t fTraversable{};
    std::uint64_t fSeed{};
    std::uint32_t fMaxBounceCount{};
    std::uint32_t fEmissionCount{};
    std::uint32_t fPhotonCount{};
    std::uint32_t fEnablePerformanceDiagnostics{};
    float fBoundaryEpsilonMm{};
};

static_assert(sizeof(DevicePhoton) == 64);
static_assert(sizeof(DevicePhotonHit) == 48);
static_assert(sizeof(DeviceOpticalEmission) == 128);

} // namespace G4GO::Optical
