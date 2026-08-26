#pragma once

#include "g4go/optical/Photon.hpp"

#include <cstdint>
#include <future>
#include <vector>

namespace G4GO::Optical {

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

} // namespace G4GO::Optical
