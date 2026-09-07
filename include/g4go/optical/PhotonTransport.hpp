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

enum class PhotonTransportStatisticField : std::uint8_t {
    Generated = 0,
    Captured,
    Detected,
    Absorbed,
    Escaped,
    Truncated,
    MaxBounce,
    InvalidState,
    ZeroStep,
    TransportTime,
};

constexpr auto StatisticFieldBit(PhotonTransportStatisticField field) -> std::uint32_t {
    return std::uint32_t{1} << static_cast<std::uint32_t>(field);
}

// Compact metadata produced by Geant4.  The GPU expands one record into its
// photons in the ray-generation program; the CPU never materialises those
// photons for the offload path.
struct alignas(16) OpticalEmission {
    std::array<float, 3> positionMm{};
    float timeNs{};

    std::array<float, 3> direction{};
    float stepLengthMm{};

    std::array<float, 3> stepDeltaMm{};
    float preVelocityMmPerNs{};

    std::array<float, 3> polarization{};
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
    std::uint32_t volumeID{InvalidID};

    std::uint32_t materialID{InvalidID};
    std::uint32_t spectrumID{};
    OpticalEmissionType type{OpticalEmissionType::Direct};
};

struct alignas(16) PhotonDetection {
    std::array<float, 3> positionMm{};
    float timeNs{};

    std::array<float, 3> direction{};
    float energyEv{};

    std::uint32_t eventID{};
    std::uint32_t photonID{};
    std::uint32_t sensorID{};
};

struct PhotonTransportStatistics {
    std::uint32_t validFields{};
    std::uint64_t generatedCount{};
    std::uint64_t capturedCount{};
    std::uint64_t detectedCount{};
    std::uint64_t absorbedCount{};
    std::uint64_t escapedCount{};
    std::uint64_t truncatedCount{};
    std::uint64_t maxBounceCount{};
    std::uint64_t invalidStateCount{};
    std::uint64_t zeroStepCount{};
    double transportTimeMs{};
};

struct PhotonTransportEventStatistics {
    std::uint32_t eventID{};
    PhotonTransportStatistics statistics{};
};

struct PhotonTransportPerformance {
    double captureTotalMs{};
    double captureLocateVolumeMs{};
    double captureVolumeMappingMs{};
    double schedulerQueueWaitMs{};
    double schedulerBatchFlattenMs{};
    std::uint64_t schedulerBatchFlattenBytes{};
    double hostToDeviceMs{};
    std::uint64_t hostToDeviceBytes{};
    double deviceMemsetMs{};
    double optiXKernelMs{};
    double deviceHitCompactionMs{};
    double deviceToHostMs{};
    double deviceMetadataToHostMs{};
    std::uint64_t deviceMetadataToHostBytes{};
    double deviceHitsToHostMs{};
    std::uint64_t deviceHitsToHostBytes{};
    double hostHitCompactionMs{};
    std::uint64_t totalBounceCount{};
    std::uint64_t coincidentCandidateTraceCount{};
    std::uint64_t coincidentCandidateHitCount{};
    std::uint64_t cerenkovPhotonCount{};
    std::uint64_t scintillationPhotonCount{};

    auto Accumulate(const PhotonTransportPerformance& other) -> void {
        captureTotalMs += other.captureTotalMs;
        captureLocateVolumeMs += other.captureLocateVolumeMs;
        captureVolumeMappingMs += other.captureVolumeMappingMs;
        schedulerQueueWaitMs += other.schedulerQueueWaitMs;
        schedulerBatchFlattenMs += other.schedulerBatchFlattenMs;
        schedulerBatchFlattenBytes += other.schedulerBatchFlattenBytes;
        hostToDeviceMs += other.hostToDeviceMs;
        hostToDeviceBytes += other.hostToDeviceBytes;
        deviceMemsetMs += other.deviceMemsetMs;
        optiXKernelMs += other.optiXKernelMs;
        deviceHitCompactionMs += other.deviceHitCompactionMs;
        deviceToHostMs += other.deviceToHostMs;
        deviceMetadataToHostMs += other.deviceMetadataToHostMs;
        deviceMetadataToHostBytes += other.deviceMetadataToHostBytes;
        deviceHitsToHostMs += other.deviceHitsToHostMs;
        deviceHitsToHostBytes += other.deviceHitsToHostBytes;
        hostHitCompactionMs += other.hostHitCompactionMs;
        totalBounceCount += other.totalBounceCount;
        coincidentCandidateTraceCount += other.coincidentCandidateTraceCount;
        coincidentCandidateHitCount += other.coincidentCandidateHitCount;
        cerenkovPhotonCount += other.cerenkovPhotonCount;
        scintillationPhotonCount += other.scintillationPhotonCount;
    }
};

struct PhotonTransportOutput {
    std::vector<PhotonDetection> detections{};
    PhotonTransportStatistics statistics{};
    PhotonTransportPerformance performance{};
    std::vector<PhotonTransportEventStatistics> eventStatistics{};
};

struct PhotonTransportBatch {
    std::span<const OpticalEmission> emissions{};
    std::span<const std::uint32_t> eventIDs{};
    std::span<const std::uint32_t> emissionEventIndices{};
};

struct OpticalSubmission {
    std::uint32_t eventID{};
    std::vector<OpticalEmission> emissions{};
    std::uint64_t photonCount{};
    PhotonTransportStatistics sourceStatistics{};
    PhotonTransportPerformance sourcePerformance{};
};

using PhotonTransportFuture = std::shared_future<PhotonTransportOutput>;

static_assert(std::is_trivially_copyable_v<PhotonDetection>);
static_assert(std::is_trivially_copyable_v<OpticalEmission>);
static_assert(sizeof(OpticalEmission) == 128);
static_assert(sizeof(PhotonDetection) == 48);

} // namespace G4GO::Optical
