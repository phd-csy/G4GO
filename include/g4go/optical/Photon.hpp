#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

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

static_assert(std::is_trivially_copyable_v<Photon>);
static_assert(std::is_trivially_copyable_v<PhotonDetection>);
static_assert(sizeof(Photon) == 64);
static_assert(sizeof(PhotonDetection) == 48);

} // namespace G4GO::Optical
