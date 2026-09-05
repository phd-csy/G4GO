#include "g4go/optical/optix/OptiXTransportHost.hpp"

#include "G4ThreeVector.hh"
#include "OptiXDeviceData.cuh"
#include "cuda_runtime_api.h"
#include "g4go/optical/optix/OptiXHitCompaction.hpp"
#include "g4go_optix_ir.h"
#include "optix_function_table_definition.h"
#include "optix_stubs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <future>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace G4GO::Optical {

namespace {

using DeviceAllocations = std::vector<std::unique_ptr<class DeviceAllocation>>;

auto DevicePointer(CUdeviceptr pointer) -> void* {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(pointer));
}

template<typename Type>
auto DevicePointerAs(CUdeviceptr pointer) -> Type* {
    return reinterpret_cast<Type*>(static_cast<std::uintptr_t>(pointer));
}

auto CudaError(cudaError_t error, const char* operation) -> void {
    if (error == cudaSuccess) {
        return;
    }
    throw std::runtime_error(std::string(operation) + " failed: " +
                             cudaGetErrorString(error));
}

struct CudaTimerPair {
    cudaEvent_t fStart{};
    cudaEvent_t fStop{};
};

auto CreateCudaTimer(CudaTimerPair& timer) -> void {
    CudaError(cudaEventCreate(&timer.fStart), "cudaEventCreate start");
    try {
        CudaError(cudaEventCreate(&timer.fStop), "cudaEventCreate stop");
    } catch (...) {
        cudaEventDestroy(timer.fStart);
        timer.fStart = nullptr;
        throw;
    }
}

auto DestroyCudaTimer(CudaTimerPair& timer) -> void {
    if (timer.fStart != nullptr) {
        cudaEventDestroy(timer.fStart);
        timer.fStart = nullptr;
    }
    if (timer.fStop != nullptr) {
        cudaEventDestroy(timer.fStop);
        timer.fStop = nullptr;
    }
}

auto RecordCudaTimerStart(const CudaTimerPair& timer,
                          cudaStream_t stream) -> void {
    if (timer.fStart != nullptr) {
        CudaError(cudaEventRecord(timer.fStart, stream),
                  "cudaEventRecord timer start");
    }
}

auto RecordCudaTimerStop(const CudaTimerPair& timer,
                         cudaStream_t stream) -> void {
    if (timer.fStop != nullptr) {
        CudaError(cudaEventRecord(timer.fStop, stream),
                  "cudaEventRecord timer stop");
    }
}

auto ReadCudaTimerMs(const CudaTimerPair& timer) -> double {
    if (timer.fStart == nullptr || timer.fStop == nullptr) {
        return 0.0;
    }
    float milliseconds{};
    CudaError(cudaEventElapsedTime(&milliseconds, timer.fStart, timer.fStop),
              "cudaEventElapsedTime");
    return static_cast<double>(milliseconds);
}

template<typename Type>
auto EnsurePinnedAllocation(Type*& allocation,
                            std::size_t& capacity,
                            std::size_t count,
                            const char* operation) -> void {
    if (capacity >= count) {
        return;
    }
    void* rawPointer{nullptr};
    CudaError(cudaMallocHost(&rawPointer, count * sizeof(Type)), operation);
    auto* replacement{static_cast<Type*>(rawPointer)};
    if (allocation != nullptr) {
        CudaError(cudaFreeHost(allocation), "cudaFreeHost");
    }
    allocation = replacement;
    capacity = count;
}

auto OptiXError(OptixResult status, const char* operation) -> void {
    if (status == OPTIX_SUCCESS) {
        return;
    }
    throw std::runtime_error(
        std::string(operation) + " failed: " + optixGetErrorName(status) +
        " (" + optixGetErrorString(status) + ")");
}

class DeviceAllocation final {
public:
    explicit DeviceAllocation(std::size_t size) :
        pointer{} {
        void* rawPointer{nullptr};
        CudaError(cudaMalloc(&rawPointer, size), "cudaMalloc");
        pointer = reinterpret_cast<CUdeviceptr>(rawPointer);
    }

    ~DeviceAllocation() {
        if (pointer != 0) {
            cudaFree(DevicePointer(pointer));
        }
    }

    DeviceAllocation(const DeviceAllocation&) = delete;
    auto operator=(const DeviceAllocation&) -> DeviceAllocation& = delete;

    auto Pointer() const -> CUdeviceptr { return pointer; }

private:
    CUdeviceptr pointer;
};

struct TransportSlot final {
    explicit TransportSlot(bool enableDiagnostics) :
        fStream{},
        fEmissionAllocation{},
        fEmissionOffsetAllocation{},
        fEmissionEventIndexAllocation{},
        fHitAllocation{},
        fHitFlagAllocation{},
        fCompactedHitAllocation{},
        fHitCountAllocation{},
        fCompactionTemporaryAllocation{},
        fStatsAllocation{},
        fEventStatsAllocation{},
        fLaunchParamsAllocation{},
        fEmissionCapacity{},
        fEmissionOffsetCapacity{},
        fEmissionEventIndexCapacity{},
        fHitCapacity{},
        fHitFlagCapacity{},
        fCompactedHitCapacity{},
        fHitCountCapacity{},
        fCompactionTemporaryCapacity{},
        fCompactionInputCapacity{},
        fStatsCapacity{},
        fEventStatsCapacity{},
        fLaunchParamsCapacity{},
        fHostEmissions{},
        fHostEmissionOffsets{},
        fHostEmissionEventIndices{},
        fHostCompactHits{},
        fHostHitCount{},
        fHostStats{},
        fHostEventStats{},
        fHostEmissionCapacity{},
        fHostEmissionOffsetCapacity{},
        fHostEmissionEventIndexCapacity{},
        fHostCompactHitCapacity{},
        fHostHitCountCapacity{},
        fHostStatsCapacity{},
        fHostEventStatsCapacity{},
        fCompactionReadyEvent{},
        fHostToDeviceTimer{},
        fDeviceMemsetTimer{},
        fOptiXKernelTimer{},
        fDeviceHitCompactionTimer{},
        fMetadataToHostTimer{},
        fHitsToHostTimer{},
        fHostLaunchParams{},
        fOutput{},
        fPhotonCount{},
        fHostToDeviceBytes{},
        fStartedAt{},
        fBusy{} {
        CudaError(cudaStreamCreateWithFlags(&fStream, cudaStreamNonBlocking),
                  "cudaStreamCreateWithFlags transport slot");
        try {
            CudaError(cudaEventCreateWithFlags(&fCompactionReadyEvent,
                                               cudaEventDisableTiming),
                      "cudaEventCreateWithFlags compaction ready");
            if (enableDiagnostics) {
                CreateCudaTimer(fHostToDeviceTimer);
                CreateCudaTimer(fDeviceMemsetTimer);
                CreateCudaTimer(fOptiXKernelTimer);
                CreateCudaTimer(fDeviceHitCompactionTimer);
                CreateCudaTimer(fMetadataToHostTimer);
                CreateCudaTimer(fHitsToHostTimer);
            }
        } catch (...) {
            Release();
            throw;
        }
    }

    ~TransportSlot() { Release(); }

    TransportSlot(const TransportSlot&) = delete;
    auto operator=(const TransportSlot&) -> TransportSlot& = delete;

    auto Release() -> void {
        if (fStream != nullptr) {
            cudaStreamSynchronize(fStream);
        }
        DestroyCudaTimer(fHostToDeviceTimer);
        DestroyCudaTimer(fDeviceMemsetTimer);
        DestroyCudaTimer(fOptiXKernelTimer);
        DestroyCudaTimer(fDeviceHitCompactionTimer);
        DestroyCudaTimer(fMetadataToHostTimer);
        DestroyCudaTimer(fHitsToHostTimer);
        if (fCompactionReadyEvent != nullptr) {
            cudaEventDestroy(fCompactionReadyEvent);
            fCompactionReadyEvent = nullptr;
        }
        if (fHostEmissions != nullptr) {
            cudaFreeHost(fHostEmissions);
            fHostEmissions = nullptr;
        }
        if (fHostEmissionOffsets != nullptr) {
            cudaFreeHost(fHostEmissionOffsets);
            fHostEmissionOffsets = nullptr;
        }
        if (fHostEmissionEventIndices != nullptr) {
            cudaFreeHost(fHostEmissionEventIndices);
            fHostEmissionEventIndices = nullptr;
        }
        if (fHostCompactHits != nullptr) {
            cudaFreeHost(fHostCompactHits);
            fHostCompactHits = nullptr;
        }
        if (fHostHitCount != nullptr) {
            cudaFreeHost(fHostHitCount);
            fHostHitCount = nullptr;
        }
        if (fHostStats != nullptr) {
            cudaFreeHost(fHostStats);
            fHostStats = nullptr;
        }
        if (fHostEventStats != nullptr) {
            cudaFreeHost(fHostEventStats);
            fHostEventStats = nullptr;
        }
        if (fStream != nullptr) {
            cudaStreamDestroy(fStream);
            fStream = nullptr;
        }
    }

    cudaStream_t fStream;
    std::unique_ptr<DeviceAllocation> fEmissionAllocation;
    std::unique_ptr<DeviceAllocation> fEmissionOffsetAllocation;
    std::unique_ptr<DeviceAllocation> fEmissionEventIndexAllocation;
    std::unique_ptr<DeviceAllocation> fHitAllocation;
    std::unique_ptr<DeviceAllocation> fHitFlagAllocation;
    std::unique_ptr<DeviceAllocation> fCompactedHitAllocation;
    std::unique_ptr<DeviceAllocation> fHitCountAllocation;
    std::unique_ptr<DeviceAllocation> fCompactionTemporaryAllocation;
    std::unique_ptr<DeviceAllocation> fStatsAllocation;
    std::unique_ptr<DeviceAllocation> fEventStatsAllocation;
    std::unique_ptr<DeviceAllocation> fLaunchParamsAllocation;
    std::size_t fEmissionCapacity;
    std::size_t fEmissionOffsetCapacity;
    std::size_t fEmissionEventIndexCapacity;
    std::size_t fHitCapacity;
    std::size_t fHitFlagCapacity;
    std::size_t fCompactedHitCapacity;
    std::size_t fHitCountCapacity;
    std::size_t fCompactionTemporaryCapacity;
    std::size_t fCompactionInputCapacity;
    std::size_t fStatsCapacity;
    std::size_t fEventStatsCapacity;
    std::size_t fLaunchParamsCapacity;
    DeviceOpticalEmission* fHostEmissions;
    std::uint32_t* fHostEmissionOffsets;
    std::uint32_t* fHostEmissionEventIndices;
    DevicePhotonHit* fHostCompactHits;
    std::uint32_t* fHostHitCount;
    DeviceTransportStats* fHostStats;
    DeviceEventTransportStats* fHostEventStats;
    std::size_t fHostEmissionCapacity;
    std::size_t fHostEmissionOffsetCapacity;
    std::size_t fHostEmissionEventIndexCapacity;
    std::size_t fHostCompactHitCapacity;
    std::size_t fHostHitCountCapacity;
    std::size_t fHostStatsCapacity;
    std::size_t fHostEventStatsCapacity;
    cudaEvent_t fCompactionReadyEvent;
    CudaTimerPair fHostToDeviceTimer;
    CudaTimerPair fDeviceMemsetTimer;
    CudaTimerPair fOptiXKernelTimer;
    CudaTimerPair fDeviceHitCompactionTimer;
    CudaTimerPair fMetadataToHostTimer;
    CudaTimerPair fHitsToHostTimer;
    OptixLaunchParams fHostLaunchParams;
    PhotonTransportOutput fOutput;
    std::size_t fPhotonCount;
    std::uint64_t fHostToDeviceBytes;
    std::chrono::steady_clock::time_point fStartedAt;
    bool fBusy;
};

template<typename Type>
auto Upload(std::span<const Type> data, DeviceAllocations& allocations)
    -> CUdeviceptr {
    if (data.empty()) {
        return 0;
    }

    auto allocation{std::make_unique<DeviceAllocation>(data.size_bytes())};
    const auto pointer{allocation->Pointer()};
    CudaError(cudaMemcpy(DevicePointer(pointer), data.data(), data.size_bytes(),
                         cudaMemcpyHostToDevice),
              "cudaMemcpy host to device");
    allocations.emplace_back(std::move(allocation));
    return pointer;
}

template<typename Type>
auto UploadObject(const Type& value, DeviceAllocations& allocations)
    -> CUdeviceptr {
    return Upload(std::span<const Type>(&value, 1), allocations);
}

auto EnsureAllocation(std::unique_ptr<DeviceAllocation>& allocation,
                      std::size_t& capacity,
                      std::size_t count,
                      std::size_t elementSize) -> void {
    if (capacity >= count) {
        return;
    }
    allocation = std::make_unique<DeviceAllocation>(count * elementSize);
    capacity = count;
}

auto UploadProperty(const PropertyTable& property,
                    DeviceAllocations& allocations) -> DeviceProperty {
    if (property.fEnergyEv.empty() ||
        property.fEnergyEv.size() != property.fValues.size()) {
        return {};
    }

    const auto energyPointer{Upload<float>(property.fEnergyEv, allocations)};
    const auto valuePointer{Upload<float>(property.fValues, allocations)};
    return {
        DevicePointerAs<const float>(energyPointer),
        DevicePointerAs<const float>(valuePointer),
        static_cast<std::uint32_t>(property.fEnergyEv.size()),
        property.Constant() ? 1U : 0U,
    };
}

auto UploadSpectrumProperty(const PropertyTable& property,
                            DeviceAllocations& allocations) -> DeviceProperty {
    if (property.fEnergyEv.empty() ||
        property.fEnergyEv.size() != property.fValues.size()) {
        return {};
    }
    if (property.fEnergyEv.size() == 1U) {
        const std::vector<float> cdf{1.0F};
        return {
            DevicePointerAs<const float>(
                Upload<float>(property.fEnergyEv, allocations)),
            DevicePointerAs<const float>(Upload<float>(cdf, allocations)),
            1U,
            0U,
        };
    }

    std::vector<float> cdf(property.fValues.size());
    for (auto index{std::size_t{1}}; index < property.fValues.size();
         ++index) {
        const auto energyDelta{property.fEnergyEv.at(index) -
                               property.fEnergyEv.at(index - 1U)};
        const auto value0{std::max(property.fValues.at(index - 1U), 0.0F)};
        const auto value1{std::max(property.fValues.at(index), 0.0F)};
        if (!(energyDelta >= 0.0F) || !std::isfinite(energyDelta) ||
            !std::isfinite(value0) || !std::isfinite(value1)) {
            return {};
        }
        cdf[index] = cdf.at(index - 1U) +
                     0.5F * energyDelta * (value0 + value1);
    }
    const auto integral{cdf.at(cdf.size() - 1U)};
    if (!(integral > 0.0F) || !std::isfinite(integral)) {
        return {};
    }
    for (auto& value : cdf) {
        value /= integral;
    }
    cdf[cdf.size() - 1U] = 1.0F;
    return {
        DevicePointerAs<const float>(
            Upload<float>(property.fEnergyEv, allocations)),
        DevicePointerAs<const float>(Upload<float>(cdf, allocations)),
        static_cast<std::uint32_t>(cdf.size()),
        0U,
    };
}

auto ToDevice(const G4ThreeVector& vector) -> DeviceVector3 {
    return {static_cast<float>(vector.x()), static_cast<float>(vector.y()),
            static_cast<float>(vector.z())};
}

static_assert(sizeof(OpticalEmission) == sizeof(DeviceOpticalEmission));

template<typename Record>
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord {
    std::uint8_t header[OPTIX_SBT_RECORD_HEADER_SIZE]{};
    Record data{};
};

struct EmptySbtData {};

using EmptySbtRecord = SbtRecord<EmptySbtData>;

static_assert(sizeof(EmptySbtRecord) % OPTIX_SBT_RECORD_ALIGNMENT == 0);

auto LogCallback(unsigned int level,
                 const char* tag,
                 const char* message,
                 void*) -> void {
    if (level <= 2) {
        std::fprintf(stderr, "[OptiX][%s] %s\n", tag, message);
    }
}

} // namespace

class OptiXTransportHost::Impl final {
public:
    explicit Impl(PhotonTransportConfig configuration) :
        fConfiguration{configuration},
        context{},
        module{},
        raygenProgram{},
        missProgram{},
        hitgroupProgram{},
        pipeline{},
        fSceneBuildStream{},
        fSlots{},
        fPendingSlots{},
        sbt{},
        sbtAllocations{},
        sceneAllocations{},
        deviceScene{},
        fVolumeIndices{},
        fVolumeIndicesByID{},
        fSceneMutex{},
        sceneAddress{},
        traversableHandle{} {
        CudaError(cudaFree(nullptr), "CUDA initialization");
        OptiXError(optixInit(), "optixInit");

        OptixDeviceContextOptions contextOptions{};
        contextOptions.logCallbackFunction = LogCallback;
        contextOptions.logCallbackLevel = 3;
        OptiXError(optixDeviceContextCreate(nullptr, &contextOptions,
                                            &context),
                   "optixDeviceContextCreate");
        CudaError(
            cudaStreamCreateWithFlags(&fSceneBuildStream, cudaStreamNonBlocking),
            "cudaStreamCreateWithFlags scene build");
        try {
            fSlots.emplace_back(std::make_unique<TransportSlot>(
                fConfiguration.fEnablePerformanceDiagnostics));
        } catch (...) {
            cudaStreamDestroy(fSceneBuildStream);
            fSceneBuildStream = nullptr;
            throw;
        }

        CreatePipeline();
        CreateSbt();
    }

    ~Impl() {
        if (fSceneBuildStream != nullptr) {
            cudaStreamSynchronize(fSceneBuildStream);
        }
        fPendingSlots.clear();
        fSlots.clear();
        if (pipeline != nullptr) {
            optixPipelineDestroy(pipeline);
        }
        if (hitgroupProgram != nullptr) {
            optixProgramGroupDestroy(hitgroupProgram);
        }
        if (missProgram != nullptr) {
            optixProgramGroupDestroy(missProgram);
        }
        if (raygenProgram != nullptr) {
            optixProgramGroupDestroy(raygenProgram);
        }
        if (module != nullptr) {
            optixModuleDestroy(module);
        }
        sbtAllocations.clear();
        sceneAllocations.clear();
        if (fSceneBuildStream != nullptr) {
            cudaStreamDestroy(fSceneBuildStream);
            fSceneBuildStream = nullptr;
        }
        if (context != nullptr) {
            optixDeviceContextDestroy(context);
        }
    }

    auto PrepareScene(const Scene& scene) -> void {
        std::lock_guard lock{fSceneMutex};
        if (sceneAddress != &scene) {
            if (!fPendingSlots.empty()) {
                throw std::logic_error(
                    "OptiX scene cannot be rebuilt with pending batches");
            }
            BuildScene(scene);
        }
    }

    auto EnqueueEmissions(const Scene& scene,
                          const PhotonTransportBatch& batch) -> void {
        PrepareScene(scene);
        const auto slotIterator{std::find_if(
            fSlots.begin(), fSlots.end(), [](const auto& candidate) {
                return candidate != nullptr && !candidate->fBusy;
            })};
        if (slotIterator == fSlots.end()) {
            throw std::logic_error("OptiX transport queue is full");
        }
        const auto slotIndex{static_cast<std::size_t>(
            std::distance(fSlots.begin(), slotIterator))};
        auto& slot{**slotIterator};
        auto& stream{slot.fStream};
        auto& fEmissionAllocation{slot.fEmissionAllocation};
        auto& fEmissionOffsetAllocation{slot.fEmissionOffsetAllocation};
        auto& fEmissionEventIndexAllocation{
            slot.fEmissionEventIndexAllocation};
        auto& fHitAllocation{slot.fHitAllocation};
        auto& fHitFlagAllocation{slot.fHitFlagAllocation};
        auto& fCompactedHitAllocation{slot.fCompactedHitAllocation};
        auto& fHitCountAllocation{slot.fHitCountAllocation};
        auto& fCompactionTemporaryAllocation{
            slot.fCompactionTemporaryAllocation};
        auto& fStatsAllocation{slot.fStatsAllocation};
        auto& fEventStatsAllocation{slot.fEventStatsAllocation};
        auto& fLaunchParamsAllocation{slot.fLaunchParamsAllocation};
        auto& fEmissionCapacity{slot.fEmissionCapacity};
        auto& fEmissionOffsetCapacity{slot.fEmissionOffsetCapacity};
        auto& fEmissionEventIndexCapacity{slot.fEmissionEventIndexCapacity};
        auto& fHitCapacity{slot.fHitCapacity};
        auto& fHitFlagCapacity{slot.fHitFlagCapacity};
        auto& fCompactedHitCapacity{slot.fCompactedHitCapacity};
        auto& fHitCountCapacity{slot.fHitCountCapacity};
        auto& fCompactionTemporaryCapacity{slot.fCompactionTemporaryCapacity};
        auto& fCompactionInputCapacity{slot.fCompactionInputCapacity};
        auto& fStatsCapacity{slot.fStatsCapacity};
        auto& fEventStatsCapacity{slot.fEventStatsCapacity};
        auto& fLaunchParamsCapacity{slot.fLaunchParamsCapacity};
        auto& fHostEmissions{slot.fHostEmissions};
        auto& fHostEmissionOffsets{slot.fHostEmissionOffsets};
        auto& fHostEmissionEventIndices{slot.fHostEmissionEventIndices};
        auto& fHostCompactHits{slot.fHostCompactHits};
        auto& fHostHitCount{slot.fHostHitCount};
        auto& fHostStats{slot.fHostStats};
        auto& fHostEventStats{slot.fHostEventStats};
        auto& fHostEmissionCapacity{slot.fHostEmissionCapacity};
        auto& fHostEmissionOffsetCapacity{slot.fHostEmissionOffsetCapacity};
        auto& fHostEmissionEventIndexCapacity{
            slot.fHostEmissionEventIndexCapacity};
        auto& fHostCompactHitCapacity{slot.fHostCompactHitCapacity};
        auto& fHostHitCountCapacity{slot.fHostHitCountCapacity};
        auto& fHostStatsCapacity{slot.fHostStatsCapacity};
        auto& fHostEventStatsCapacity{slot.fHostEventStatsCapacity};
        auto& fCompactionReadyEvent{slot.fCompactionReadyEvent};
        auto& fHostToDeviceTimer{slot.fHostToDeviceTimer};
        auto& fDeviceMemsetTimer{slot.fDeviceMemsetTimer};
        auto& fOptiXKernelTimer{slot.fOptiXKernelTimer};
        auto& fDeviceHitCompactionTimer{slot.fDeviceHitCompactionTimer};
        auto& fMetadataToHostTimer{slot.fMetadataToHostTimer};

        PhotonTransportOutput output{};
        const auto emissions{batch.fEmissions};
        if (batch.fEventIDs.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error(
                "OptiX event batch exceeds the supported event count");
        }
        if (batch.fEmissionEventIndices.size() != emissions.size()) {
            throw std::invalid_argument(
                "OptiX emission event-index count does not match emissions");
        }
        std::unordered_map<std::uint32_t, std::uint32_t> eventIndices{};
        eventIndices.reserve(batch.fEventIDs.size());
        for (auto index{std::size_t{}}; index < batch.fEventIDs.size();
             ++index) {
            if (!eventIndices.emplace(batch.fEventIDs[index],
                                      static_cast<std::uint32_t>(index))
                     .second) {
                throw std::invalid_argument(
                    "OptiX event batch contains duplicate event IDs");
            }
            output.fEventStatistics.emplace_back(
                PhotonTransportEventStatistics{batch.fEventIDs[index], {}});
        }
        for (auto index{std::size_t{}}; index < emissions.size(); ++index) {
            const auto eventIndex{batch.fEmissionEventIndices[index]};
            if (eventIndex >= batch.fEventIDs.size() ||
                emissions[index].fEventID != batch.fEventIDs[eventIndex]) {
                throw std::invalid_argument(
                    "OptiX emission event index does not match event ID");
            }
            auto& eventStatistics{
                output.fEventStatistics.at(eventIndex).fStatistics};
            eventStatistics.fCapturedCount += emissions[index].fPhotonCount;
            eventStatistics.fValidFields |= StatisticFieldBit(
                PhotonTransportStatisticField::Captured);
        }
        if (emissions.empty()) {
            slot.fOutput = std::move(output);
            slot.fPhotonCount = 0;
            slot.fStartedAt = std::chrono::steady_clock::now();
            slot.fBusy = true;
            fPendingSlots.emplace_back(slotIndex);
            return;
        }
        if (emissions.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error(
                "OptiX emission batch exceeds the supported launch size");
        }

        std::uint64_t photonCount{};
        for (const auto& emission : emissions) {
            photonCount += emission.fPhotonCount;
        }
        if (photonCount == 0) {
            for (auto& eventOutput : output.fEventStatistics) {
                eventOutput.fStatistics.fValidFields |=
                    StatisticFieldBit(PhotonTransportStatisticField::Detected) |
                    StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
                    StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
                    StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
                    StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
                    StatisticFieldBit(
                        PhotonTransportStatisticField::InvalidState) |
                    StatisticFieldBit(PhotonTransportStatisticField::ZeroStep);
            }
            slot.fOutput = std::move(output);
            slot.fPhotonCount = 0;
            slot.fStartedAt = std::chrono::steady_clock::now();
            slot.fBusy = true;
            fPendingSlots.emplace_back(slotIndex);
            return;
        }
        if (photonCount > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error(
                "OptiX emission batch exceeds the supported photon count");
        }
        output.fStatistics.fCapturedCount = photonCount;
        output.fStatistics.fValidFields =
            StatisticFieldBit(PhotonTransportStatisticField::Captured);
        slot.fHostToDeviceBytes =
            emissions.size() * sizeof(DeviceOpticalEmission) +
            (emissions.size() + 1U) * sizeof(std::uint32_t) +
            emissions.size() * sizeof(std::uint32_t) +
            sizeof(OptixLaunchParams);

        const auto start{std::chrono::steady_clock::now()};
        EnsurePinnedAllocation(fHostEmissions, fHostEmissionCapacity,
                               emissions.size(), "cudaMallocHost emissions");
        EnsurePinnedAllocation(fHostEmissionOffsets,
                               fHostEmissionOffsetCapacity,
                               emissions.size() + 1U,
                               "cudaMallocHost emission offsets");
        EnsurePinnedAllocation(fHostEmissionEventIndices,
                               fHostEmissionEventIndexCapacity,
                               emissions.size(),
                               "cudaMallocHost emission event indices");
        fHostEmissionOffsets[0] = 0;
        std::uint64_t offset{};
        for (auto index{std::size_t{}}; index < emissions.size(); ++index) {
            const auto& emission{emissions[index]};
            auto deviceEmission{
                std::bit_cast<DeviceOpticalEmission>(emission)};
            auto deviceVolumeID{InvalidID};
            if (emission.fVolumeID < fVolumeIndicesByID.size()) {
                deviceVolumeID =
                    fVolumeIndicesByID.at(emission.fVolumeID);
            } else if (const auto volumeIndex{
                           fVolumeIndices.find(emission.fVolumeID)};
                       volumeIndex != fVolumeIndices.end()) {
                deviceVolumeID = volumeIndex->second;
            }
            if (deviceVolumeID == InvalidID) {
                throw std::invalid_argument(
                    "emission references an unknown scene volume ID");
            }
            if (emission.fMaterialID >= scene.Materials().size()) {
                throw std::invalid_argument(
                    "emission references an unknown scene material ID");
            }
            deviceEmission.fVolumeID = deviceVolumeID;
            fHostEmissions[index] = deviceEmission;
            fHostEmissionEventIndices[index] =
                batch.fEmissionEventIndices[index];
            offset += emission.fPhotonCount;
            fHostEmissionOffsets[index + 1U] =
                static_cast<std::uint32_t>(offset);
        }

        EnsureAllocation(fEmissionAllocation, fEmissionCapacity,
                         emissions.size(), sizeof(DeviceOpticalEmission));
        EnsureAllocation(fEmissionOffsetAllocation, fEmissionOffsetCapacity,
                         emissions.size() + 1U, sizeof(std::uint32_t));
        EnsureAllocation(fEmissionEventIndexAllocation,
                         fEmissionEventIndexCapacity, emissions.size(),
                         sizeof(std::uint32_t));
        EnsureAllocation(fHitAllocation, fHitCapacity,
                         static_cast<std::size_t>(photonCount),
                         sizeof(DevicePhotonHit));
        EnsureAllocation(fHitFlagAllocation, fHitFlagCapacity,
                         static_cast<std::size_t>(photonCount),
                         sizeof(std::uint32_t));
        EnsureAllocation(fCompactedHitAllocation, fCompactedHitCapacity,
                         static_cast<std::size_t>(photonCount),
                         sizeof(DevicePhotonHit));
        EnsureAllocation(fHitCountAllocation, fHitCountCapacity, 1,
                         sizeof(std::uint32_t));
        EnsureAllocation(fStatsAllocation, fStatsCapacity, 1,
                         sizeof(DeviceTransportStats));
        EnsureAllocation(fEventStatsAllocation, fEventStatsCapacity,
                         batch.fEventIDs.size(),
                         sizeof(DeviceEventTransportStats));
        EnsureAllocation(fLaunchParamsAllocation, fLaunchParamsCapacity, 1,
                         sizeof(OptixLaunchParams));
        EnsurePinnedAllocation(fHostCompactHits, fHostCompactHitCapacity,
                               static_cast<std::size_t>(photonCount),
                               "cudaMallocHost compact hits");
        EnsurePinnedAllocation(fHostHitCount, fHostHitCountCapacity, 1,
                               "cudaMallocHost hit count");
        EnsurePinnedAllocation(fHostStats, fHostStatsCapacity, 1,
                               "cudaMallocHost transport stats");
        EnsurePinnedAllocation(fHostEventStats, fHostEventStatsCapacity,
                               batch.fEventIDs.size(),
                               "cudaMallocHost event transport stats");

        if (fCompactionInputCapacity < static_cast<std::size_t>(photonCount)) {
            const auto compactionBytes{QueryOptiXHitCompactionBytes(
                static_cast<std::size_t>(photonCount))};
            if (compactionBytes == 0) {
                throw std::runtime_error(
                    "unable to determine OptiX hit compaction temporary "
                    "storage");
            }
            EnsureAllocation(fCompactionTemporaryAllocation,
                             fCompactionTemporaryCapacity, compactionBytes, 1);
            fCompactionInputCapacity = static_cast<std::size_t>(photonCount);
        }

        RecordCudaTimerStart(fHostToDeviceTimer, stream);
        CudaError(cudaMemcpyAsync(
                      DevicePointer(fEmissionAllocation->Pointer()),
                      fHostEmissions,
                      emissions.size() * sizeof(DeviceOpticalEmission),
                      cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync emissions");
        CudaError(cudaMemcpyAsync(
                      DevicePointer(fEmissionOffsetAllocation->Pointer()),
                      fHostEmissionOffsets,
                      (emissions.size() + 1U) * sizeof(std::uint32_t),
                      cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync emission offsets");
        CudaError(cudaMemcpyAsync(
                      DevicePointer(fEmissionEventIndexAllocation->Pointer()),
                      fHostEmissionEventIndices,
                      emissions.size() * sizeof(std::uint32_t),
                      cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync emission event indices");

        OptixLaunchParams launchParams{};
        launchParams.fEmissions = DevicePointerAs<DeviceOpticalEmission>(
            fEmissionAllocation->Pointer());
        launchParams.fEmissionEventIndices =
            DevicePointerAs<const std::uint32_t>(
                fEmissionEventIndexAllocation->Pointer());
        launchParams.fEmissionOffsets = DevicePointerAs<const std::uint32_t>(
            fEmissionOffsetAllocation->Pointer());
        launchParams.fHits = DevicePointerAs<DevicePhotonHit>(
            fHitAllocation->Pointer());
        launchParams.fHitFlags = DevicePointerAs<std::uint32_t>(
            fHitFlagAllocation->Pointer());
        launchParams.fStats =
            DevicePointerAs<DeviceTransportStats>(fStatsAllocation->Pointer());
        launchParams.fEventStats = DevicePointerAs<DeviceEventTransportStats>(
            fEventStatsAllocation->Pointer());
        launchParams.fScene = deviceScene;
        launchParams.fTraversable = traversableHandle;
        launchParams.fSeed = fConfiguration.fSeed;
        launchParams.fMaxBounceCount = fConfiguration.fMaxBouncesPerPhoton;
        launchParams.fEmissionCount = static_cast<std::uint32_t>(emissions.size());
        launchParams.fEventCount =
            static_cast<std::uint32_t>(batch.fEventIDs.size());
        launchParams.fPhotonCount = static_cast<std::uint32_t>(photonCount);
        launchParams.fEnablePerformanceDiagnostics =
            fConfiguration.fEnablePerformanceDiagnostics ? 1U : 0U;
        launchParams.fBoundaryEpsilonMm =
            fConfiguration.fBoundaryToleranceMm;
        slot.fHostLaunchParams = launchParams;
        CudaError(cudaMemcpyAsync(
                      DevicePointer(fLaunchParamsAllocation->Pointer()),
                      &slot.fHostLaunchParams, sizeof(slot.fHostLaunchParams),
                      cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync launch params");
        RecordCudaTimerStop(fHostToDeviceTimer, stream);

        RecordCudaTimerStart(fDeviceMemsetTimer, stream);
        CudaError(cudaMemsetAsync(DevicePointer(fStatsAllocation->Pointer()),
                                  0, sizeof(DeviceTransportStats), stream),
                  "cudaMemsetAsync transport stats");
        CudaError(cudaMemsetAsync(
                      DevicePointer(fEventStatsAllocation->Pointer()), 0,
                      batch.fEventIDs.size() * sizeof(DeviceEventTransportStats),
                      stream),
                  "cudaMemsetAsync event transport stats");
        CudaError(cudaMemsetAsync(DevicePointer(fHitFlagAllocation->Pointer()),
                                  0,
                                  static_cast<std::size_t>(photonCount) *
                                      sizeof(std::uint32_t),
                                  stream),
                  "cudaMemsetAsync hit flags");
        RecordCudaTimerStop(fDeviceMemsetTimer, stream);

        RecordCudaTimerStart(fOptiXKernelTimer, stream);
        OptiXError(optixLaunch(
                       pipeline, stream, fLaunchParamsAllocation->Pointer(),
                       sizeof(launchParams), &sbt,
                       static_cast<unsigned int>(photonCount), 1, 1),
                   "optixLaunch");
        RecordCudaTimerStop(fOptiXKernelTimer, stream);
        RecordCudaTimerStart(fDeviceHitCompactionTimer, stream);
        CudaError(CompactOptiXHits(
                      fHitAllocation->Pointer(), fHitFlagAllocation->Pointer(),
                      fCompactedHitAllocation->Pointer(),
                      fHitCountAllocation->Pointer(),
                      fCompactionTemporaryAllocation->Pointer(),
                      fCompactionTemporaryCapacity,
                      static_cast<std::size_t>(photonCount), stream),
                  "OptiX hit compaction");
        RecordCudaTimerStop(fDeviceHitCompactionTimer, stream);

        RecordCudaTimerStart(fMetadataToHostTimer, stream);
        CudaError(cudaMemcpyAsync(
                      fHostStats, DevicePointer(fStatsAllocation->Pointer()),
                      sizeof(DeviceTransportStats), cudaMemcpyDeviceToHost,
                      stream),
                  "cudaMemcpyAsync transport stats");
        CudaError(cudaMemcpyAsync(
                      fHostEventStats,
                      DevicePointer(fEventStatsAllocation->Pointer()),
                      batch.fEventIDs.size() * sizeof(DeviceEventTransportStats),
                      cudaMemcpyDeviceToHost, stream),
                  "cudaMemcpyAsync event transport stats");
        CudaError(cudaMemcpyAsync(
                      fHostHitCount,
                      DevicePointer(fHitCountAllocation->Pointer()),
                      sizeof(std::uint32_t), cudaMemcpyDeviceToHost, stream),
                  "cudaMemcpyAsync hit count");
        RecordCudaTimerStop(fMetadataToHostTimer, stream);
        CudaError(cudaEventRecord(fCompactionReadyEvent, stream),
                  "cudaEventRecord compaction ready");

        slot.fOutput = std::move(output);
        slot.fPhotonCount = static_cast<std::size_t>(photonCount);
        slot.fStartedAt = start;
        slot.fBusy = true;
        fPendingSlots.emplace_back(slotIndex);
    }

    auto CompleteOldestBatch() -> PhotonTransportOutput {
        if (fPendingSlots.empty()) {
            throw std::logic_error("OptiX transport queue is empty");
        }
        const auto slotIndex{fPendingSlots.front()};
        auto& slot{*fSlots.at(slotIndex)};
        auto& stream{slot.fStream};
        auto& output{slot.fOutput};
        const auto photonCount{slot.fPhotonCount};

        if (photonCount == 0) {
            auto completedOutput{std::move(output)};
            slot.fOutput = {};
            slot.fBusy = false;
            fPendingSlots.pop_front();
            return completedOutput;
        }

        CudaError(cudaEventSynchronize(slot.fCompactionReadyEvent),
                  "cudaEventSynchronize compaction ready");
        const auto compactHitCount{
            static_cast<std::size_t>(*slot.fHostHitCount)};
        if (compactHitCount > photonCount) {
            throw std::runtime_error(
                "OptiX hit compaction returned an invalid hit count");
        }
        RecordCudaTimerStart(slot.fHitsToHostTimer, stream);
        if (compactHitCount > 0) {
            CudaError(cudaMemcpyAsync(
                          slot.fHostCompactHits,
                          DevicePointer(
                              slot.fCompactedHitAllocation->Pointer()),
                          compactHitCount * sizeof(DevicePhotonHit),
                          cudaMemcpyDeviceToHost, stream),
                      "cudaMemcpyAsync compact hits");
        }
        RecordCudaTimerStop(slot.fHitsToHostTimer, stream);
        CudaError(cudaStreamSynchronize(stream), "cudaStreamSynchronize");

        const auto& deviceStats{*slot.fHostStats};
        output.fStatistics.fDetectedCount = deviceStats.fDetectedCount;
        output.fStatistics.fAbsorbedCount = deviceStats.fAbsorbedCount;
        output.fStatistics.fEscapedCount = deviceStats.fEscapedCount;
        output.fStatistics.fTruncatedCount = deviceStats.fTruncatedCount;
        output.fStatistics.fMaxBounceCount = deviceStats.fMaxBounceCount;
        output.fStatistics.fInvalidStateCount =
            deviceStats.fInvalidStateCount;
        output.fStatistics.fZeroStepCount = deviceStats.fZeroStepCount;
        output.fStatistics.fTransportTimeMs =
            std::chrono::duration<double, std::milli>{
                std::chrono::steady_clock::now() - slot.fStartedAt}
                .count();
        output.fStatistics.fValidFields |=
            StatisticFieldBit(PhotonTransportStatisticField::Detected) |
            StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
            StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
            StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
            StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
            StatisticFieldBit(PhotonTransportStatisticField::InvalidState) |
            StatisticFieldBit(PhotonTransportStatisticField::ZeroStep) |
            StatisticFieldBit(PhotonTransportStatisticField::TransportTime);
        for (auto index{std::size_t{}};
             index < output.fEventStatistics.size(); ++index) {
            auto& eventStatistics{
                output.fEventStatistics.at(index).fStatistics};
            const auto& deviceEventStatistics{slot.fHostEventStats[index]};
            eventStatistics.fDetectedCount =
                deviceEventStatistics.fDetectedCount;
            eventStatistics.fAbsorbedCount =
                deviceEventStatistics.fAbsorbedCount;
            eventStatistics.fEscapedCount = deviceEventStatistics.fEscapedCount;
            eventStatistics.fTruncatedCount =
                deviceEventStatistics.fTruncatedCount;
            eventStatistics.fMaxBounceCount =
                deviceEventStatistics.fMaxBounceCount;
            eventStatistics.fInvalidStateCount =
                deviceEventStatistics.fInvalidStateCount;
            eventStatistics.fZeroStepCount =
                deviceEventStatistics.fZeroStepCount;
            eventStatistics.fValidFields |=
                StatisticFieldBit(PhotonTransportStatisticField::Detected) |
                StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
                StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
                StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
                StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
                StatisticFieldBit(
                    PhotonTransportStatisticField::InvalidState) |
                StatisticFieldBit(PhotonTransportStatisticField::ZeroStep);
        }
        output.fPerformance.fHostToDeviceMs =
            ReadCudaTimerMs(slot.fHostToDeviceTimer);
        output.fPerformance.fHostToDeviceBytes = slot.fHostToDeviceBytes;
        output.fPerformance.fDeviceMemsetMs =
            ReadCudaTimerMs(slot.fDeviceMemsetTimer);
        output.fPerformance.fOptiXKernelMs =
            ReadCudaTimerMs(slot.fOptiXKernelTimer);
        output.fPerformance.fDeviceHitCompactionMs =
            ReadCudaTimerMs(slot.fDeviceHitCompactionTimer);
        output.fPerformance.fDeviceMetadataToHostMs =
            ReadCudaTimerMs(slot.fMetadataToHostTimer);
        output.fPerformance.fDeviceMetadataToHostBytes =
            sizeof(DeviceTransportStats) + sizeof(std::uint32_t) +
            output.fEventStatistics.size() *
                sizeof(DeviceEventTransportStats);
        output.fPerformance.fDeviceHitsToHostMs =
            ReadCudaTimerMs(slot.fHitsToHostTimer);
        output.fPerformance.fDeviceHitsToHostBytes =
            compactHitCount * sizeof(DevicePhotonHit);
        output.fPerformance.fDeviceToHostMs =
            output.fPerformance.fDeviceMetadataToHostMs +
            output.fPerformance.fDeviceHitsToHostMs;
        output.fPerformance.fTotalBounceCount = deviceStats.fTotalBounceCount;
        output.fPerformance.fCoincidentCandidateTraceCount =
            deviceStats.fCoincidentCandidateTraceCount;
        output.fPerformance.fCoincidentCandidateHitCount =
            deviceStats.fCoincidentCandidateHitCount;
        const auto hostCompactionStart{std::chrono::steady_clock::now()};
        output.fDetections.reserve(compactHitCount);
        for (auto index{std::size_t{}}; index < compactHitCount; ++index) {
            const auto& hit{slot.fHostCompactHits[index]};
            output.fDetections.emplace_back(PhotonDetection{
                {hit.fPositionMm.at(0), hit.fPositionMm.at(1),
                 hit.fPositionMm.at(2)},
                hit.fTimeNs,
                {hit.fDirection.at(0),  hit.fDirection.at(1),
                 hit.fDirection.at(2) },
                hit.fEnergyEv,
                hit.fEventID,
                hit.fPhotonID,
                hit.fSensorID,
                hit.fFlags,
            });
        }
        output.fPerformance.fHostHitCompactionMs =
            std::chrono::duration<double, std::milli>{
                std::chrono::steady_clock::now() - hostCompactionStart}
                .count();
        auto completedOutput{std::move(output)};
        slot.fOutput = {};
        slot.fPhotonCount = 0;
        slot.fHostToDeviceBytes = 0;
        slot.fBusy = false;
        fPendingSlots.pop_front();
        return completedOutput;
    }

    auto PendingBatchCount() const -> std::size_t {
        return fPendingSlots.size();
    }

    auto PropagateEmissions(const Scene& scene,
                            const PhotonTransportBatch& batch)
        -> PhotonTransportOutput {
        if (!fPendingSlots.empty()) {
            throw std::logic_error(
                "synchronous OptiX transport cannot run with pending batches");
        }
        EnqueueEmissions(scene, batch);
        return CompleteOldestBatch();
    }

private:
    auto CreatePipeline() -> void {
        OptixModuleCompileOptions moduleOptions{};
        moduleOptions.maxRegisterCount =
            OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
        moduleOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
        moduleOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;

        OptixPipelineCompileOptions pipelineOptions{};
        pipelineOptions.traversableGraphFlags =
            OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
        pipelineOptions.numPayloadValues = 6;
        pipelineOptions.numAttributeValues = 2;
        pipelineOptions.pipelineLaunchParamsVariableName = "gLaunchParams";
        pipelineOptions.pipelineLaunchParamsSizeInBytes =
            sizeof(OptixLaunchParams);
        pipelineOptions.usesPrimitiveTypeFlags =
            OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;

        std::array<char, 8192> log{};
        auto logSize{log.size()};
        const auto moduleStatus{optixModuleCreate(
            context, &moduleOptions, &pipelineOptions,
            reinterpret_cast<const char*>(g4go_optix_ir),
            static_cast<std::size_t>(g4go_optix_irLength), log.data(), &logSize,
            &module)};
        if (moduleStatus != OPTIX_SUCCESS) {
            throw std::runtime_error(
                std::string("optixModuleCreate failed: ") +
                optixGetErrorName(moduleStatus) + " (" +
                optixGetErrorString(moduleStatus) + ") " +
                std::string(log.data(), logSize));
        }

        OptixProgramGroupOptions programOptions{};
        OptixProgramGroupDesc raygenDescription{};
        raygenDescription.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        raygenDescription.raygen.module = module;
        raygenDescription.raygen.entryFunctionName = "__raygen__rg";
        raygenProgram = CreateProgramGroup(raygenDescription, programOptions,
                                           "raygen program group");

        OptixProgramGroupDesc missDescription{};
        missDescription.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        missDescription.miss.module = module;
        missDescription.miss.entryFunctionName = "__miss__ms";
        missProgram = CreateProgramGroup(missDescription, programOptions,
                                         "miss program group");

        OptixProgramGroupDesc hitgroupDescription{};
        hitgroupDescription.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        hitgroupDescription.hitgroup.moduleCH = module;
        hitgroupDescription.hitgroup.entryFunctionNameCH = "__closesthit__ch";
        hitgroupDescription.hitgroup.moduleAH = module;
        hitgroupDescription.hitgroup.entryFunctionNameAH = "__anyhit__ah";
        hitgroupProgram = CreateProgramGroup(hitgroupDescription, programOptions,
                                             "hitgroup program group");

        const std::array programGroups{
            raygenProgram,
            missProgram,
            hitgroupProgram,
        };
        OptixPipelineLinkOptions linkOptions{};
        linkOptions.maxTraceDepth = 1;
        linkOptions.maxTraversableGraphDepth = 2;
        log.fill(0);
        logSize = log.size();
        const auto pipelineStatus{optixPipelineCreate(
            context, &pipelineOptions, &linkOptions, programGroups.data(),
            static_cast<unsigned int>(programGroups.size()), log.data(),
            &logSize, &pipeline)};
        if (pipelineStatus != OPTIX_SUCCESS) {
            throw std::runtime_error(
                std::string("optixPipelineCreate failed: ") +
                optixGetErrorName(pipelineStatus) + " (" +
                optixGetErrorString(pipelineStatus) + ") " +
                std::string(log.data(), logSize));
        }
        OptiXError(optixPipelineSetStackSizeFromCallDepths(
                       pipeline, 1, 0, 0, 0, 2),
                   "optixPipelineSetStackSizeFromCallDepths");
    }

    auto CreateProgramGroup(const OptixProgramGroupDesc& description,
                            const OptixProgramGroupOptions& options,
                            const char* operation) -> OptixProgramGroup {
        OptixProgramGroup programGroup{};
        std::array<char, 4096> log{};
        auto logSize{log.size()};
        const auto status{optixProgramGroupCreate(
            context, &description, 1, &options, log.data(), &logSize,
            &programGroup)};
        if (status != OPTIX_SUCCESS) {
            throw std::runtime_error(
                std::string(operation) + " failed: " +
                optixGetErrorName(status) + " (" + optixGetErrorString(status) +
                ") " + std::string(log.data(), logSize));
        }
        return programGroup;
    }

    auto CreateSbt() -> void {
        EmptySbtRecord raygenRecord{};
        EmptySbtRecord missRecord{};
        EmptySbtRecord hitgroupRecord{};
        OptiXError(optixSbtRecordPackHeader(raygenProgram, &raygenRecord),
                   "optixSbtRecordPackHeader raygen");
        OptiXError(optixSbtRecordPackHeader(missProgram, &missRecord),
                   "optixSbtRecordPackHeader miss");
        OptiXError(optixSbtRecordPackHeader(hitgroupProgram, &hitgroupRecord),
                   "optixSbtRecordPackHeader hitgroup");

        sbt = {};
        sbt.raygenRecord = UploadObject(raygenRecord, sbtAllocations);
        sbt.missRecordBase = UploadObject(missRecord, sbtAllocations);
        sbt.missRecordStrideInBytes = sizeof(EmptySbtRecord);
        sbt.missRecordCount = 1;
        sbt.hitgroupRecordBase =
            UploadObject(hitgroupRecord, sbtAllocations);
        sbt.hitgroupRecordStrideInBytes = sizeof(EmptySbtRecord);
        sbt.hitgroupRecordCount = 1;
    }

    auto BuildScene(const Scene& scene) -> void {
        if (scene.Geometries().empty() || scene.Volumes().empty()) {
            throw std::invalid_argument("OptiX scene cannot be empty");
        }
        if (scene.Volumes().size() >= InvalidID) {
            throw std::overflow_error("OptiX scene contains too many volumes");
        }

        std::unordered_map<std::uint32_t, std::uint32_t> volumeIndices{};
        volumeIndices.reserve(scene.Volumes().size());
        for (auto index{std::size_t{}}; index < scene.Volumes().size(); ++index) {
            const auto volumeID{scene.Volumes().at(index).fVolumeID};
            if (volumeID == InvalidID ||
                !volumeIndices.emplace(volumeID, static_cast<std::uint32_t>(index))
                     .second) {
                throw std::invalid_argument(
                    "OptiX scene volume IDs must be unique and valid");
            }
        }
        const auto mapVolumeID{
            [&](std::uint32_t volumeID, const char* relationship) {
                if (volumeID == InvalidID) {
                    return InvalidID;
                }
                const auto volumeIndex{volumeIndices.find(volumeID)};
                if (volumeIndex == volumeIndices.end()) {
                    throw std::invalid_argument(
                        std::string("OptiX scene references an unknown ") +
                        relationship + " volume ID");
                }
                return volumeIndex->second;
            }};
        const auto mapRequiredVolumeID{
            [&](std::uint32_t volumeID, const char* relationship) {
                if (volumeID == InvalidID) {
                    throw std::invalid_argument(
                        std::string("OptiX scene has no ") + relationship +
                        " volume ID");
                }
                return mapVolumeID(volumeID, relationship);
            }};
        sceneAllocations.clear();

        std::vector<DeviceMaterial> materials{};
        materials.reserve(scene.Materials().size());
        for (const auto& material : scene.Materials()) {
            materials.emplace_back(DeviceMaterial{
                UploadProperty(material.fRindex, sceneAllocations),
                material.fRindexMax,
                UploadProperty(material.fGroupVelocityMmPerNs,
                               sceneAllocations),
                UploadProperty(material.fAbsLengthMm, sceneAllocations),
                {
                                                           UploadSpectrumProperty(material.fScintillationSpectrum.at(0),
                                                           sceneAllocations),
                                                           UploadSpectrumProperty(material.fScintillationSpectrum.at(1),
                                                           sceneAllocations),
                                                           UploadSpectrumProperty(material.fScintillationSpectrum.at(2),
                                                           sceneAllocations),
                                                           },
            });
        }

        std::vector<DeviceSurface> surfaces{};
        surfaces.reserve(scene.Surfaces().size());
        for (const auto& surface : scene.Surfaces()) {
            surfaces.emplace_back(DeviceSurface{
                static_cast<std::uint8_t>(surface.fType),
                static_cast<std::uint8_t>(surface.fModel),
                static_cast<std::uint8_t>(surface.fFinish),
                0,
                surface.fModelValue,
                UploadProperty(surface.fReflectivity, sceneAllocations),
                UploadProperty(surface.fEfficiency, sceneAllocations),
                UploadProperty(surface.fTransmittance, sceneAllocations),
                UploadProperty(surface.fRindex, sceneAllocations),
                UploadProperty(surface.fSpecularLobe, sceneAllocations),
                UploadProperty(surface.fSpecularSpike, sceneAllocations),
                UploadProperty(surface.fBackscatter, sceneAllocations),
                UploadProperty(surface.fSurfaceRoughness, sceneAllocations),
            });
        }

        std::vector<DeviceGeometry> geometries{};
        geometries.reserve(scene.Geometries().size());
        std::vector<OptixTraversableHandle> geometryHandles{};
        geometryHandles.reserve(scene.Geometries().size());
        for (const auto& geometry : scene.Geometries()) {
            const auto& mesh{geometry.fMesh};
            if (mesh.fVerticesMm.empty() || mesh.fIndices.empty() ||
                mesh.fIndices.size() % 3 != 0) {
                throw std::invalid_argument("OptiX scene contains invalid mesh " +
                                            geometry.fName);
            }
            if (std::any_of(mesh.fIndices.begin(), mesh.fIndices.end(),
                            [&](const auto index) {
                                return index >= mesh.fVerticesMm.size();
                            })) {
                throw std::invalid_argument(
                    "OptiX scene mesh index exceeds vertex count: " +
                    geometry.fName);
            }
            if (!mesh.fTriangleFlags.empty() &&
                mesh.fTriangleFlags.size() != mesh.fIndices.size() / 3) {
                throw std::invalid_argument(
                    "OptiX scene triangle flag count mismatch: " +
                    geometry.fName);
            }
            const auto triangleCount{mesh.fIndices.size() / 3};
            std::vector<G4ThreeVector> triangleNormals{};
            if (mesh.fTriangleNormals.empty()) {
                triangleNormals.reserve(triangleCount);
                for (auto triangle{std::size_t{}}; triangle < triangleCount;
                     ++triangle) {
                    const auto base{3 * triangle};
                    const auto& first{
                        mesh.fVerticesMm.at(mesh.fIndices.at(base))};
                    const auto& second{
                        mesh.fVerticesMm.at(mesh.fIndices.at(base + 1))};
                    const auto& third{
                        mesh.fVerticesMm.at(mesh.fIndices.at(base + 2))};
                    const auto normal{(second - first).cross(third - first)};
                    if (!(normal.mag2() > 0.0)) {
                        throw std::invalid_argument(
                            "OptiX scene contains a degenerate triangle: " +
                            geometry.fName);
                    }
                    triangleNormals.emplace_back(normal.unit());
                }
            } else {
                if (mesh.fTriangleNormals.size() != triangleCount) {
                    throw std::invalid_argument(
                        "OptiX scene triangle normal count mismatch: " +
                        geometry.fName);
                }
                triangleNormals.reserve(triangleCount);
                for (const auto& normal : mesh.fTriangleNormals) {
                    if (!(normal.mag2() > 0.0)) {
                        throw std::invalid_argument(
                            "OptiX scene contains an invalid triangle normal: " +
                            geometry.fName);
                    }
                    triangleNormals.emplace_back(normal.unit());
                }
            }
            std::vector<DeviceVector3> vertices{};
            vertices.reserve(mesh.fVerticesMm.size());
            for (const auto& vertex : mesh.fVerticesMm) {
                vertices.emplace_back(ToDevice(vertex));
            }
            const auto vertexPointer{
                Upload<DeviceVector3>(vertices, sceneAllocations)};
            const auto indexPointer{
                Upload<std::uint32_t>(mesh.fIndices, sceneAllocations)};
            std::vector<DeviceVector3> deviceNormals{};
            deviceNormals.reserve(triangleNormals.size());
            for (const auto& normal : triangleNormals) {
                deviceNormals.emplace_back(ToDevice(normal));
            }
            const auto normalPointer{
                Upload<DeviceVector3>(deviceNormals, sceneAllocations)};
            const auto flagPointer{
                Upload<std::uint8_t>(mesh.fTriangleFlags, sceneAllocations)};
            geometries.emplace_back(DeviceGeometry{
                {
                 DevicePointerAs<const DeviceVector3>(vertexPointer),
                 DevicePointerAs<const std::uint32_t>(indexPointer),
                 DevicePointerAs<const DeviceVector3>(normalPointer),
                 DevicePointerAs<const std::uint8_t>(flagPointer),
                 static_cast<std::uint32_t>(mesh.fVerticesMm.size()),
                 static_cast<std::uint32_t>(triangleCount),
                 },
            });

            const CUdeviceptr vertexBuffers[]{vertexPointer};
            const unsigned int geometryFlags[]{OPTIX_GEOMETRY_FLAG_NONE};
            OptixBuildInput buildInput{};
            buildInput.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
            buildInput.triangleArray.vertexBuffers = vertexBuffers;
            buildInput.triangleArray.numVertices =
                static_cast<unsigned int>(mesh.fVerticesMm.size());
            buildInput.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
            buildInput.triangleArray.indexBuffer = indexPointer;
            buildInput.triangleArray.numIndexTriplets =
                static_cast<unsigned int>(mesh.fIndices.size() / 3);
            buildInput.triangleArray.indexFormat =
                OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
            buildInput.triangleArray.flags = geometryFlags;
            buildInput.triangleArray.numSbtRecords = 1;

            OptixAccelBuildOptions buildOptions{};
            buildOptions.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
            buildOptions.operation = OPTIX_BUILD_OPERATION_BUILD;
            buildOptions.motionOptions.numKeys = 1;
            OptixAccelBufferSizes bufferSizes{};
            OptiXError(optixAccelComputeMemoryUsage(
                           context, &buildOptions, &buildInput, 1,
                           &bufferSizes),
                       "optixAccelComputeMemoryUsage geometry");
            auto temporary{
                std::make_unique<DeviceAllocation>(bufferSizes.tempSizeInBytes)};
            auto output{
                std::make_unique<DeviceAllocation>(bufferSizes.outputSizeInBytes)};
            OptixTraversableHandle geometryHandle{};
            OptiXError(optixAccelBuild(
                           context, fSceneBuildStream, &buildOptions,
                           &buildInput, 1,
                           temporary->Pointer(), bufferSizes.tempSizeInBytes,
                           output->Pointer(), bufferSizes.outputSizeInBytes,
                           &geometryHandle, nullptr, 0),
                       "optixAccelBuild geometry");
            CudaError(cudaStreamSynchronize(fSceneBuildStream),
                      "cudaStreamSynchronize geometry GAS");
            sceneAllocations.emplace_back(std::move(temporary));
            sceneAllocations.emplace_back(std::move(output));
            geometryHandles.emplace_back(geometryHandle);
        }

        std::vector<DeviceVolume> volumes{};
        volumes.reserve(scene.Volumes().size());
        std::vector<OptixInstance> instances{};
        instances.reserve(scene.Volumes().size());
        for (auto index{std::size_t{}}; index < scene.Volumes().size(); ++index) {
            const auto& volume{scene.Volumes().at(index)};
            if (volume.fGeometryID >= geometryHandles.size()) {
                throw std::invalid_argument("volume references invalid geometry");
            }
            volumes.emplace_back(DeviceVolume{
                static_cast<std::uint32_t>(index),
                volume.fPhysicalVolumeID,
                volume.fCopyNo,
                volume.fGeometryID,
                volume.fMaterialID,
                mapVolumeID(volume.fParentVolumeID, "parent"),
                volume.fSkinSurfaceID,
                volume.fSensorID,
                volume.fDepth,
                static_cast<std::uint8_t>(volume.fMayHaveCoincidentBoundary),
                {},
            });
            const auto& rotation{volume.fTransform.fRotation};
            const auto& translation{volume.fTransform.fTranslationMm};
            OptixInstance instance{};
            instance.transform[0] = rotation.fXX;
            instance.transform[1] = rotation.fXY;
            instance.transform[2] = rotation.fXZ;
            instance.transform[3] = static_cast<float>(translation.x());
            instance.transform[4] = rotation.fYX;
            instance.transform[5] = rotation.fYY;
            instance.transform[6] = rotation.fYZ;
            instance.transform[7] = static_cast<float>(translation.y());
            instance.transform[8] = rotation.fZX;
            instance.transform[9] = rotation.fZY;
            instance.transform[10] = rotation.fZZ;
            instance.transform[11] = static_cast<float>(translation.z());
            instance.instanceId = static_cast<unsigned int>(index);
            instance.visibilityMask = 255;
            instance.flags = OPTIX_INSTANCE_FLAG_DISABLE_TRIANGLE_FACE_CULLING;
            instance.traversableHandle = geometryHandles.at(volume.fGeometryID);
            instances.emplace_back(instance);
        }

        std::vector<DeviceSurfaceBinding> deviceBindings{};
        deviceBindings.reserve(scene.SurfaceBindings().size());
        for (const auto& binding : scene.SurfaceBindings()) {
            deviceBindings.emplace_back(DeviceSurfaceBinding{
                mapRequiredVolumeID(binding.fFromVolumeID, "boundary source"),
                mapRequiredVolumeID(binding.fToVolumeID, "boundary target"),
                binding.fSurfaceID,
            });
        }
        std::sort(deviceBindings.begin(), deviceBindings.end(),
                  [](const auto& left, const auto& right) {
                      if (left.fFromVolumeID != right.fFromVolumeID) {
                          return left.fFromVolumeID < right.fFromVolumeID;
                      }
                      return left.fToVolumeID < right.fToVolumeID;
                  });

        const auto materialPointer{Upload<DeviceMaterial>(
            materials, sceneAllocations)};
        const auto surfacePointer{Upload<DeviceSurface>(
            surfaces, sceneAllocations)};
        const auto geometryPointer{Upload<DeviceGeometry>(
            geometries, sceneAllocations)};
        const auto volumePointer{Upload<DeviceVolume>(volumes, sceneAllocations)};
        const auto bindingPointer{Upload<DeviceSurfaceBinding>(
            deviceBindings, sceneAllocations)};
        const auto instancePointer{Upload<OptixInstance>(
            instances, sceneAllocations)};

        deviceScene = {
            DevicePointerAs<const DeviceMaterial>(materialPointer),
            static_cast<std::uint32_t>(materials.size()),
            DevicePointerAs<const DeviceSurface>(surfacePointer),
            static_cast<std::uint32_t>(surfaces.size()),
            DevicePointerAs<const DeviceGeometry>(geometryPointer),
            static_cast<std::uint32_t>(geometries.size()),
            DevicePointerAs<const DeviceVolume>(volumePointer),
            static_cast<std::uint32_t>(volumes.size()),
            DevicePointerAs<const DeviceSurfaceBinding>(bindingPointer),
            static_cast<std::uint32_t>(deviceBindings.size()),
            mapRequiredVolumeID(scene.WorldVolumeID(), "world"),
        };

        OptixBuildInput instanceInput{};
        instanceInput.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
        instanceInput.instanceArray.instances = instancePointer;
        instanceInput.instanceArray.numInstances =
            static_cast<unsigned int>(instances.size());
        OptixAccelBuildOptions instanceOptions{};
        instanceOptions.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
        instanceOptions.operation = OPTIX_BUILD_OPERATION_BUILD;
        OptixAccelBufferSizes instanceBufferSizes{};
        OptiXError(optixAccelComputeMemoryUsage(
                       context, &instanceOptions, &instanceInput, 1,
                       &instanceBufferSizes),
                   "optixAccelComputeMemoryUsage IAS");
        auto instanceTemporary{std::make_unique<DeviceAllocation>(
            instanceBufferSizes.tempSizeInBytes)};
        auto instanceOutput{std::make_unique<DeviceAllocation>(
            instanceBufferSizes.outputSizeInBytes)};
        OptiXError(optixAccelBuild(
                       context, fSceneBuildStream, &instanceOptions,
                       &instanceInput, 1,
                       instanceTemporary->Pointer(),
                       instanceBufferSizes.tempSizeInBytes,
                       instanceOutput->Pointer(),
                       instanceBufferSizes.outputSizeInBytes,
                       &traversableHandle, nullptr, 0),
                   "optixAccelBuild IAS");
        CudaError(cudaStreamSynchronize(fSceneBuildStream),
                  "cudaStreamSynchronize scene IAS");
        sceneAllocations.emplace_back(std::move(instanceTemporary));
        sceneAllocations.emplace_back(std::move(instanceOutput));
        if (std::all_of(
                volumeIndices.begin(), volumeIndices.end(),
                [&](const auto& entry) {
                    return entry.first < scene.Volumes().size();
                })) {
            fVolumeIndicesByID.assign(scene.Volumes().size(), InvalidID);
            for (const auto& [volumeID, volumeIndex] : volumeIndices) {
                fVolumeIndicesByID[volumeID] = volumeIndex;
            }
        } else {
            fVolumeIndicesByID.clear();
        }
        fVolumeIndices = std::move(volumeIndices);
        sceneAddress = &scene;
    }

    PhotonTransportConfig fConfiguration;
    OptixDeviceContext context;
    OptixModule module;
    OptixProgramGroup raygenProgram;
    OptixProgramGroup missProgram;
    OptixProgramGroup hitgroupProgram;
    OptixPipeline pipeline;
    cudaStream_t fSceneBuildStream;
    std::vector<std::unique_ptr<TransportSlot>> fSlots;
    std::deque<std::size_t> fPendingSlots;
    OptixShaderBindingTable sbt;
    DeviceAllocations sbtAllocations;
    DeviceAllocations sceneAllocations;
    DeviceScene deviceScene;
    std::unordered_map<std::uint32_t, std::uint32_t> fVolumeIndices;
    std::vector<std::uint32_t> fVolumeIndicesByID;
    std::mutex fSceneMutex;
    const Scene* sceneAddress;
    OptixTraversableHandle traversableHandle;
};

OptiXTransportHost::OptiXTransportHost(
    PhotonTransportConfig configuration) :
    fImpl{std::make_unique<Impl>(configuration)} {}

OptiXTransportHost::~OptiXTransportHost() = default;

auto OptiXTransportHost::PrepareScene(const Scene& scene) -> void {
    fImpl->PrepareScene(scene);
}

auto OptiXTransportHost::EnqueueEmissions(
    const Scene& scene, const PhotonTransportBatch& batch) -> void {
    fImpl->EnqueueEmissions(scene, batch);
}

auto OptiXTransportHost::CompleteOldestBatch() -> PhotonTransportOutput {
    return fImpl->CompleteOldestBatch();
}

auto OptiXTransportHost::PendingBatchCount() const -> std::size_t {
    return fImpl->PendingBatchCount();
}

auto OptiXTransportHost::PropagateEmissions(
    const Scene& scene, const PhotonTransportBatch& batch)
    -> PhotonTransportOutput {
    return fImpl->PropagateEmissions(scene, batch);
}

} // namespace G4GO::Optical
