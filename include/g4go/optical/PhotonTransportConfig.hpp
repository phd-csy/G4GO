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
    PhotonTransportBackend backend{PhotonTransportBackend::Auto};
    std::uint64_t seed{};
    std::uint32_t maxPhotonsPerEvent{5'000'000};
    std::uint32_t maxBouncesPerPhoton{4096};
    float boundaryToleranceMm{1.0e-4F};
    std::uint32_t targetPhotonsPerBatch{1'000'000};
    std::uint32_t maxEventsPerBatch{10'000};
    std::uint32_t maxQueuedPhotons{2'000'000};
    std::uint32_t maxQueuedSubmissions{1'024};
    std::uint32_t maxPendingEvents{64};
    std::uint32_t batchCollectionTimeoutMs{10};
    std::uint32_t meshRotationSteps{360};
    bool enablePerformanceDiagnostics{};
};

} // namespace G4GO::Optical
