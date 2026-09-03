#pragma once

#include "g4go/optical/Scene.hpp"

#include <array>
#include <cstdint>
#include <future>
#include <span>
#include <type_traits>
#include <vector>

namespace G4GO::Optical {

enum class OpticalEmissionType : std::uint8_t {
    Direct,
    Cerenkov,
    Scintillation,
};

// Compact metadata produced by Geant4.  The GPU expands one record into its
// photons in the ray-generation program; the CPU never materialises those
// photons for the offload path.
struct alignas(16) OpticalEmission {
    std::array<float, 3> fPositionMm{};
    float fTimeNs{};

    std::array<float, 3> fDirection{};
    float fStepLengthMm{};

    std::array<float, 3> fStepDeltaMm{};
    float fPreVelocityMmPerNs{};

    std::array<float, 3> fPolarization{};
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
    std::uint32_t fVolumeID{InvalidID};

    std::uint32_t fMaterialID{InvalidID};
    std::uint32_t fSpectrumID{};
    OpticalEmissionType fType{OpticalEmissionType::Direct};
    std::uint8_t fFlags{};
    std::uint16_t fReserved{};
};

struct alignas(16) PhotonDetection {
    std::array<float, 3> fPositionMm{};
    float fTimeNs{};

    std::array<float, 3> fDirection{};
    float fEnergyEv{};

    std::uint32_t fEventID{};
    std::uint32_t fPhotonID{};
    std::uint32_t fSensorID{};
    std::uint32_t fFlags{};
};

struct PhotonTransportStatistics {
    std::uint64_t fGeneratedCount{};
    std::uint64_t fCapturedCount{};
    std::uint64_t fDetectedCount{};
    std::uint64_t fAbsorbedCount{};
    std::uint64_t fEscapedCount{};
    std::uint64_t fTruncatedCount{};
    std::uint64_t fMaxBounceCount{};
    std::uint64_t fInvalidStateCount{};
    std::uint64_t fZeroStepCount{};
    double fTransportTimeMs{};
};

struct PhotonTransportPerformance {
    double fCaptureTotalMs{};
    double fCaptureLocateVolumeMs{};
    double fCaptureVolumeMappingMs{};
    double fSchedulerQueueWaitMs{};
    double fSchedulerBatchFlattenMs{};
    std::uint64_t fSchedulerBatchFlattenBytes{};
    double fHostToDeviceMs{};
    double fDeviceMemsetMs{};
    double fOptiXKernelMs{};
    double fDeviceHitCompactionMs{};
    double fDeviceToHostMs{};
    double fHostHitCompactionMs{};
    std::uint64_t fTotalBounceCount{};
    std::uint64_t fCoincidentCandidateTraceCount{};
    std::uint64_t fCoincidentCandidateHitCount{};
    std::uint64_t fCerenkovPhotonCount{};
    std::uint64_t fScintillationPhotonCount{};

    auto Accumulate(const PhotonTransportPerformance& other) -> void {
        fCaptureTotalMs += other.fCaptureTotalMs;
        fCaptureLocateVolumeMs += other.fCaptureLocateVolumeMs;
        fCaptureVolumeMappingMs += other.fCaptureVolumeMappingMs;
        fSchedulerQueueWaitMs += other.fSchedulerQueueWaitMs;
        fSchedulerBatchFlattenMs += other.fSchedulerBatchFlattenMs;
        fSchedulerBatchFlattenBytes += other.fSchedulerBatchFlattenBytes;
        fHostToDeviceMs += other.fHostToDeviceMs;
        fDeviceMemsetMs += other.fDeviceMemsetMs;
        fOptiXKernelMs += other.fOptiXKernelMs;
        fDeviceHitCompactionMs += other.fDeviceHitCompactionMs;
        fDeviceToHostMs += other.fDeviceToHostMs;
        fHostHitCompactionMs += other.fHostHitCompactionMs;
        fTotalBounceCount += other.fTotalBounceCount;
        fCoincidentCandidateTraceCount +=
            other.fCoincidentCandidateTraceCount;
        fCoincidentCandidateHitCount += other.fCoincidentCandidateHitCount;
        fCerenkovPhotonCount += other.fCerenkovPhotonCount;
        fScintillationPhotonCount += other.fScintillationPhotonCount;
    }
};

struct PhotonTransportOutput {
    std::vector<PhotonDetection> fDetections{};
    PhotonTransportStatistics fStatistics{};
    PhotonTransportPerformance fPerformance{};
};

struct OpticalSubmission {
    std::uint32_t fEventID{};
    std::vector<OpticalEmission> fEmissions{};
    std::uint64_t fPhotonCount{};
    PhotonTransportStatistics fSourceStatistics{};
    PhotonTransportPerformance fSourcePerformance{};
};

using PhotonTransportFuture = std::shared_future<PhotonTransportOutput>;

static_assert(std::is_trivially_copyable_v<PhotonDetection>);
static_assert(std::is_trivially_copyable_v<OpticalEmission>);
static_assert(sizeof(OpticalEmission) == 128);
static_assert(sizeof(PhotonDetection) == 48);

class PhotonTransport {
public:
    PhotonTransport() = default;
    virtual ~PhotonTransport() = default;

    PhotonTransport(const PhotonTransport&) = delete;
    auto operator=(const PhotonTransport&) -> PhotonTransport& = delete;

    virtual auto PropagateEmissions(
        const Scene& scene, std::span<const OpticalEmission> emissions)
        -> PhotonTransportOutput = 0;
};

} // namespace G4GO::Optical
