#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace G4GO::Optical {

enum class Backend : std::uint8_t {
    Auto,
    Geant4,
    Capture,
    Cpu,
    Optix,
};

constexpr auto BackendName(Backend backend) -> std::string_view {
    switch (backend) {
    case Backend::Auto:
        return "auto";
    case Backend::Geant4:
        return "geant4";
    case Backend::Capture:
        return "capture";
    case Backend::Cpu:
        return "cpu";
    case Backend::Optix:
        return "optix";
    }
    return "unknown";
}

enum class PhotonSource : std::uint8_t {
    Unknown,
    Scintillation,
    Cerenkov,
};

struct Vector3 {
    float fX{};
    float fY{};
    float fZ{};
};

struct alignas(16) Photon {
    Vector3 fPositionMm{};
    float fTimeNs{};

    Vector3 fDirection{};
    float fEnergyEv{};

    Vector3 fPolarization{};
    float fWeight{1.0F};

    std::uint64_t fPhotonID{};
    std::uint32_t fVolumeID{};
    PhotonSource fSource{PhotonSource::Unknown};
    std::uint8_t fFlags{};
    std::uint16_t fReserved{};
};

struct alignas(16) PhotonHit {
    Vector3 fPositionMm{};
    float fTimeNs{};

    Vector3 fDirection{};
    float fEnergyEv{};

    std::uint64_t fPhotonID{};
    std::uint32_t fSensorID{};
    std::uint32_t fFlags{};
};

struct TransportStats {
    std::uint64_t fGeneratedCount{};
    std::uint64_t fCapturedCount{};
    std::uint64_t fDetectedCount{};
    std::uint64_t fAbsorbedCount{};
    std::uint64_t fEscapedCount{};
    std::uint64_t fMaxBounceCount{};
    std::uint64_t fInvalidStateCount{};
    std::uint64_t fZeroStepCount{};
    double fTransportTimeMs{};
};

struct TransportResult {
    std::vector<PhotonHit> fHitData{};
    TransportStats fStats{};
};

struct TransportConfig {
    Backend fBackend{Backend::Auto};
    std::uint64_t fSeed{42};
    std::uint32_t fMaxPhotonCount{5'000'000};
    std::uint32_t fMaxBounceCount{4096};
    float fBoundaryEpsilonMm{1.0e-4F};
};

static_assert(std::is_trivially_copyable_v<Photon>);
static_assert(std::is_trivially_copyable_v<PhotonHit>);
static_assert(sizeof(Photon) == 64);
static_assert(sizeof(PhotonHit) == 48);

} // namespace G4GO::Optical
