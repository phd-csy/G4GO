#pragma once

#include <cstdint>
#include <string_view>

namespace G4GO::Optical {

enum class PhotonTransportBackend : std::uint8_t {
    Auto,
    Geant4,
    OptiX,
};

constexpr auto ToString(PhotonTransportBackend backend) -> std::string_view {
    switch (backend) {
    case PhotonTransportBackend::Auto:
        return "auto";
    case PhotonTransportBackend::Geant4:
        return "cpu";
    case PhotonTransportBackend::OptiX:
        return "gpu";
    }
    return "unknown";
}

struct PhotonTransportConfig {
    PhotonTransportBackend fBackend{PhotonTransportBackend::Auto};
    std::uint64_t fSeed{42};
    std::uint32_t fMaxPhotonsPerEvent{5'000'000};
    std::uint32_t fMaxBouncesPerPhoton{4096};
    float fBoundaryToleranceMm{1.0e-4F};
    std::uint32_t fTargetPhotonsPerBatch{1'000'000};
    std::uint32_t fMaxEventsPerBatch{10'000};
    std::uint32_t fMaxQueuedPhotons{2'000'000};
    std::uint32_t fMaxQueuedSubmissions{1'024};
    std::uint32_t fMaxPendingEvents{64};
    std::uint32_t fBatchCollectionTimeoutMs{15};
    std::uint32_t fMaxInFlightBatches{2};
    std::uint32_t fMeshRotationSteps{360};
    bool fEnablePerformanceDiagnostics{};
};

} // namespace G4GO::Optical
