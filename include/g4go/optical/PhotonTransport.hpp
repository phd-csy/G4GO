#pragma once

#include "g4go/optical/Scene.hpp"

#include <array>
#include <cstdint>
#include <future>
#include <span>
#include <type_traits>
#include <vector>

namespace G4GO::Optical {

enum class PhotonSource : std::uint8_t {
    Unknown,
    Scintillation,
    Cerenkov,
};

struct alignas(16) Photon {
    std::array<float, 3> fPositionMm{};
    float fTimeNs{};

    std::array<float, 3> fDirection{};
    float fEnergyEv{};

    std::array<float, 3> fPolarization{};
    float fWeight{1.0F};

    std::uint32_t fEventID{};
    std::uint32_t fPhotonID{};
    std::uint32_t fVolumeID{};
    PhotonSource fSource{PhotonSource::Unknown};
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

struct PhotonTransportOutput {
    std::vector<PhotonDetection> fDetections{};
    PhotonTransportStatistics fStatistics{};
};

using PhotonTransportFuture = std::shared_future<PhotonTransportOutput>;

static_assert(std::is_trivially_copyable_v<Photon>);
static_assert(std::is_trivially_copyable_v<PhotonDetection>);
static_assert(sizeof(Photon) == 64);
static_assert(sizeof(PhotonDetection) == 48);

class PhotonTransport {
public:
    PhotonTransport() = default;
    virtual ~PhotonTransport() = default;

    PhotonTransport(const PhotonTransport&) = delete;
    auto operator=(const PhotonTransport&) -> PhotonTransport& = delete;

    virtual auto Propagate(const Scene& scene, std::span<const Photon> photons)
        -> PhotonTransportOutput = 0;
};

} // namespace G4GO::Optical
