#pragma once

#include "cuda.h"
#include "cuda_runtime_api.h"

#include <cstddef>

namespace G4GO::Optical {

auto QueryOptiXHitCompactionBytes(std::size_t count) -> std::size_t;

auto CompactOptiXHits(CUdeviceptr inputHits, CUdeviceptr inputFlags, CUdeviceptr outputHits, CUdeviceptr outputCount,
                      CUdeviceptr temporaryStorage, std::size_t temporaryStorageBytes, std::size_t count,
                      cudaStream_t stream) -> cudaError_t;

} // namespace G4GO::Optical
