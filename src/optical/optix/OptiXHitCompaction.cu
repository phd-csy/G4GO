#include "OptiXDeviceData.cuh"
#include "g4go/optical/optix/OptiXHitCompaction.hpp"

#include <cub/device/device_select.cuh>

namespace G4GO::Optical {

auto QueryOptiXHitCompactionBytes(std::size_t count) -> std::size_t {
    if (count == 0) {
        return 0;
    }

    std::size_t bytes{};
    const auto error{cub::DeviceSelect::Flagged(
        nullptr, bytes, static_cast<const DevicePhotonHit*>(nullptr),
        static_cast<const std::uint32_t*>(nullptr),
        static_cast<DevicePhotonHit*>(nullptr),
        static_cast<std::uint32_t*>(nullptr), count, nullptr)};
    if (error != cudaSuccess) {
        return 0;
    }
    return bytes;
}

auto CompactOptiXHits(CUdeviceptr inputHits,
                      CUdeviceptr inputFlags,
                      CUdeviceptr outputHits,
                      CUdeviceptr outputCount,
                      CUdeviceptr temporaryStorage,
                      std::size_t temporaryStorageBytes,
                      std::size_t count,
                      cudaStream_t stream) -> cudaError_t {
    return cub::DeviceSelect::Flagged(
        reinterpret_cast<void*>(temporaryStorage), temporaryStorageBytes,
        reinterpret_cast<const DevicePhotonHit*>(inputHits),
        reinterpret_cast<const std::uint32_t*>(inputFlags),
        reinterpret_cast<DevicePhotonHit*>(outputHits),
        reinterpret_cast<std::uint32_t*>(outputCount), count, stream);
}

} // namespace G4GO::Optical
