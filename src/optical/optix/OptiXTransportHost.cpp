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
    throw std::runtime_error(std::string(operation) + " failed: " + cudaGetErrorString(error));
}

struct CudaTimerPair {
    cudaEvent_t start{};
    cudaEvent_t stop{};
};

auto CreateCudaTimer(CudaTimerPair& timer) -> void {
    CudaError(cudaEventCreate(&timer.start), "cudaEventCreate start");
    try {
        CudaError(cudaEventCreate(&timer.stop), "cudaEventCreate stop");
    } catch (...) {
        cudaEventDestroy(timer.start);
        timer.start = nullptr;
        throw;
    }
}

auto DestroyCudaTimer(CudaTimerPair& timer) -> void {
    if (timer.start != nullptr) {
        cudaEventDestroy(timer.start);
        timer.start = nullptr;
    }
    if (timer.stop != nullptr) {
        cudaEventDestroy(timer.stop);
        timer.stop = nullptr;
    }
}

auto RecordCudaTimerStart(const CudaTimerPair& timer, cudaStream_t stream) -> void {
    if (timer.start != nullptr) {
        CudaError(cudaEventRecord(timer.start, stream), "cudaEventRecord timer start");
    }
}

auto RecordCudaTimerStop(const CudaTimerPair& timer, cudaStream_t stream) -> void {
    if (timer.stop != nullptr) {
        CudaError(cudaEventRecord(timer.stop, stream), "cudaEventRecord timer stop");
    }
}

auto ReadCudaTimerMs(const CudaTimerPair& timer) -> double {
    if (timer.start == nullptr || timer.stop == nullptr) {
        return 0.0;
    }
    float milliseconds{};
    CudaError(cudaEventElapsedTime(&milliseconds, timer.start, timer.stop), "cudaEventElapsedTime");
    return static_cast<double>(milliseconds);
}

template<typename Type>
auto EnsurePinnedAllocation(Type*& allocation, std::size_t& capacity, std::size_t count,
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
    throw std::runtime_error(std::string(operation) + " failed: " + optixGetErrorName(status) + " (" +
                             optixGetErrorString(status) + ")");
}

class DeviceAllocation final {
public:
    explicit DeviceAllocation(std::size_t size) :
        fPointer{} {
        void* rawPointer{nullptr};
        CudaError(cudaMalloc(&rawPointer, size), "cudaMalloc");
        fPointer = reinterpret_cast<CUdeviceptr>(rawPointer);
    }

    ~DeviceAllocation() {
        if (fPointer != 0) {
            cudaFree(DevicePointer(fPointer));
        }
    }

    DeviceAllocation(const DeviceAllocation&) = delete;
    auto operator=(const DeviceAllocation&) -> DeviceAllocation& = delete;

    auto Pointer() const -> CUdeviceptr { return fPointer; }

private:
    CUdeviceptr fPointer;
};

struct TransportSlot final {
    explicit TransportSlot(bool enableDiagnostics) :
        stream{},
        emissionAllocation{},
        emissionOffsetAllocation{},
        emissionEventIndexAllocation{},
        hitAllocation{},
        hitFlagAllocation{},
        compactedHitAllocation{},
        hitCountAllocation{},
        compactionTemporaryAllocation{},
        statsAllocation{},
        eventStatsAllocation{},
        launchParamsAllocation{},
        emissionCapacity{},
        emissionOffsetCapacity{},
        emissionEventIndexCapacity{},
        hitCapacity{},
        hitFlagCapacity{},
        compactedHitCapacity{},
        hitCountCapacity{},
        compactionTemporaryCapacity{},
        compactionInputCapacity{},
        statsCapacity{},
        eventStatsCapacity{},
        launchParamsCapacity{},
        hostEmissions{},
        hostEmissionOffsets{},
        hostEmissionEventIndices{},
        hostCompactHits{},
        hostHitCount{},
        hostStats{},
        hostEventStats{},
        hostEmissionCapacity{},
        hostEmissionOffsetCapacity{},
        hostEmissionEventIndexCapacity{},
        hostCompactHitCapacity{},
        hostHitCountCapacity{},
        hostStatsCapacity{},
        hostEventStatsCapacity{},
        compactionReadyEvent{},
        hostToDeviceTimer{},
        deviceMemsetTimer{},
        optiXKernelTimer{},
        deviceHitCompactionTimer{},
        metadataToHostTimer{},
        hitsToHostTimer{},
        hostLaunchParams{},
        output{},
        photonCount{},
        hostToDeviceBytes{},
        startedAt{},
        busy{} {
        CudaError(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
                  "cudaStreamCreateWithFlags transport slot");
        try {
            CudaError(cudaEventCreateWithFlags(&compactionReadyEvent, cudaEventDisableTiming),
                      "cudaEventCreateWithFlags compaction ready");
            if (enableDiagnostics) {
                CreateCudaTimer(hostToDeviceTimer);
                CreateCudaTimer(deviceMemsetTimer);
                CreateCudaTimer(optiXKernelTimer);
                CreateCudaTimer(deviceHitCompactionTimer);
                CreateCudaTimer(metadataToHostTimer);
                CreateCudaTimer(hitsToHostTimer);
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
        if (stream != nullptr) {
            cudaStreamSynchronize(stream);
        }
        DestroyCudaTimer(hostToDeviceTimer);
        DestroyCudaTimer(deviceMemsetTimer);
        DestroyCudaTimer(optiXKernelTimer);
        DestroyCudaTimer(deviceHitCompactionTimer);
        DestroyCudaTimer(metadataToHostTimer);
        DestroyCudaTimer(hitsToHostTimer);
        if (compactionReadyEvent != nullptr) {
            cudaEventDestroy(compactionReadyEvent);
            compactionReadyEvent = nullptr;
        }
        if (hostEmissions != nullptr) {
            cudaFreeHost(hostEmissions);
            hostEmissions = nullptr;
        }
        if (hostEmissionOffsets != nullptr) {
            cudaFreeHost(hostEmissionOffsets);
            hostEmissionOffsets = nullptr;
        }
        if (hostEmissionEventIndices != nullptr) {
            cudaFreeHost(hostEmissionEventIndices);
            hostEmissionEventIndices = nullptr;
        }
        if (hostCompactHits != nullptr) {
            cudaFreeHost(hostCompactHits);
            hostCompactHits = nullptr;
        }
        if (hostHitCount != nullptr) {
            cudaFreeHost(hostHitCount);
            hostHitCount = nullptr;
        }
        if (hostStats != nullptr) {
            cudaFreeHost(hostStats);
            hostStats = nullptr;
        }
        if (hostEventStats != nullptr) {
            cudaFreeHost(hostEventStats);
            hostEventStats = nullptr;
        }
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
            stream = nullptr;
        }
    }

    cudaStream_t stream;
    std::unique_ptr<DeviceAllocation> emissionAllocation;
    std::unique_ptr<DeviceAllocation> emissionOffsetAllocation;
    std::unique_ptr<DeviceAllocation> emissionEventIndexAllocation;
    std::unique_ptr<DeviceAllocation> hitAllocation;
    std::unique_ptr<DeviceAllocation> hitFlagAllocation;
    std::unique_ptr<DeviceAllocation> compactedHitAllocation;
    std::unique_ptr<DeviceAllocation> hitCountAllocation;
    std::unique_ptr<DeviceAllocation> compactionTemporaryAllocation;
    std::unique_ptr<DeviceAllocation> statsAllocation;
    std::unique_ptr<DeviceAllocation> eventStatsAllocation;
    std::unique_ptr<DeviceAllocation> launchParamsAllocation;
    std::size_t emissionCapacity;
    std::size_t emissionOffsetCapacity;
    std::size_t emissionEventIndexCapacity;
    std::size_t hitCapacity;
    std::size_t hitFlagCapacity;
    std::size_t compactedHitCapacity;
    std::size_t hitCountCapacity;
    std::size_t compactionTemporaryCapacity;
    std::size_t compactionInputCapacity;
    std::size_t statsCapacity;
    std::size_t eventStatsCapacity;
    std::size_t launchParamsCapacity;
    DeviceOpticalEmission* hostEmissions;
    std::uint32_t* hostEmissionOffsets;
    std::uint32_t* hostEmissionEventIndices;
    DevicePhotonHit* hostCompactHits;
    std::uint32_t* hostHitCount;
    DeviceTransportStats* hostStats;
    DeviceEventTransportStats* hostEventStats;
    std::size_t hostEmissionCapacity;
    std::size_t hostEmissionOffsetCapacity;
    std::size_t hostEmissionEventIndexCapacity;
    std::size_t hostCompactHitCapacity;
    std::size_t hostHitCountCapacity;
    std::size_t hostStatsCapacity;
    std::size_t hostEventStatsCapacity;
    cudaEvent_t compactionReadyEvent;
    CudaTimerPair hostToDeviceTimer;
    CudaTimerPair deviceMemsetTimer;
    CudaTimerPair optiXKernelTimer;
    CudaTimerPair deviceHitCompactionTimer;
    CudaTimerPair metadataToHostTimer;
    CudaTimerPair hitsToHostTimer;
    OptixLaunchParams hostLaunchParams;
    PhotonTransportOutput output;
    std::size_t photonCount;
    std::uint64_t hostToDeviceBytes;
    std::chrono::steady_clock::time_point startedAt;
    bool busy;
};

template<typename Type>
auto Upload(std::span<const Type> data, DeviceAllocations& allocations) -> CUdeviceptr {
    if (data.empty()) {
        return 0;
    }

    auto allocation{std::make_unique<DeviceAllocation>(data.size_bytes())};
    const auto pointer{allocation->Pointer()};
    CudaError(cudaMemcpy(DevicePointer(pointer), data.data(), data.size_bytes(), cudaMemcpyHostToDevice),
              "cudaMemcpy host to device");
    allocations.emplace_back(std::move(allocation));
    return pointer;
}

template<typename Type>
auto UploadObject(const Type& value, DeviceAllocations& allocations) -> CUdeviceptr {
    return Upload(std::span<const Type>(&value, 1), allocations);
}

auto EnsureAllocation(std::unique_ptr<DeviceAllocation>& allocation, std::size_t& capacity, std::size_t count,
                      std::size_t elementSize) -> void {
    if (capacity >= count) {
        return;
    }
    allocation = std::make_unique<DeviceAllocation>(count * elementSize);
    capacity = count;
}

auto UploadProperty(const PropertyTable& property, DeviceAllocations& allocations) -> DeviceProperty {
    if (property.energyEv.empty() || property.energyEv.size() != property.values.size()) {
        return {};
    }

    const auto energyPointer{Upload<float>(property.energyEv, allocations)};
    const auto valuePointer{Upload<float>(property.values, allocations)};
    return {
        DevicePointerAs<const float>(energyPointer),
        DevicePointerAs<const float>(valuePointer),
        static_cast<std::uint32_t>(property.energyEv.size()),
        property.Constant() ? 1U : 0U,
    };
}

auto UploadSpectrumProperty(const PropertyTable& property, DeviceAllocations& allocations) -> DeviceProperty {
    if (property.energyEv.empty() || property.energyEv.size() != property.values.size()) {
        return {};
    }
    if (property.energyEv.size() == 1U) {
        const std::vector<float> cdf{1.0F};
        return {
            DevicePointerAs<const float>(Upload<float>(property.energyEv, allocations)),
            DevicePointerAs<const float>(Upload<float>(cdf, allocations)),
            1U,
            0U,
        };
    }

    std::vector<float> cdf(property.values.size());
    for (auto index{std::size_t{1}}; index < property.values.size(); ++index) {
        const auto energyDelta{property.energyEv.at(index) - property.energyEv.at(index - 1U)};
        const auto value0{std::max(property.values.at(index - 1U), 0.0F)};
        const auto value1{std::max(property.values.at(index), 0.0F)};
        if (!(energyDelta >= 0.0F) || !std::isfinite(energyDelta) || !std::isfinite(value0) || !std::isfinite(value1)) {
            return {};
        }
        cdf[index] = cdf.at(index - 1U) + 0.5F * energyDelta * (value0 + value1);
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
        DevicePointerAs<const float>(Upload<float>(property.energyEv, allocations)),
        DevicePointerAs<const float>(Upload<float>(cdf, allocations)),
        static_cast<std::uint32_t>(cdf.size()),
        0U,
    };
}

auto ToDevice(const G4ThreeVector& vector) -> DeviceVector3 {
    return {static_cast<float>(vector.x()), static_cast<float>(vector.y()), static_cast<float>(vector.z())};
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

auto LogCallback(unsigned int level, const char* tag, const char* message, void*) -> void {
    if (level <= 2) {
        std::fprintf(stderr, "[OptiX][%s] %s\n", tag, message);
    }
}

} // namespace

class OptiXTransportHost::Impl final {
public:
    explicit Impl(PhotonTransportConfig configuration) :
        fConfiguration{configuration},
        fContext{},
        fModule{},
        fRaygenProgram{},
        fMissProgram{},
        fHitgroupProgram{},
        fPipeline{},
        fSceneBuildStream{},
        fSlot{},
        fSbt{},
        fSbtAllocations{},
        fSceneAllocations{},
        fDeviceScene{},
        fVolumeIndices{},
        fVolumeIndicesByID{},
        fSceneMutex{},
        fSceneAddress{},
        fTraversableHandle{} {
        CudaError(cudaFree(nullptr), "CUDA initialization");
        OptiXError(optixInit(), "optixInit");

        OptixDeviceContextOptions contextOptions{};
        contextOptions.logCallbackFunction = LogCallback;
        contextOptions.logCallbackLevel = 3;
        OptiXError(optixDeviceContextCreate(nullptr, &contextOptions, &fContext), "optixDeviceContextCreate");
        CudaError(cudaStreamCreateWithFlags(&fSceneBuildStream, cudaStreamNonBlocking),
                  "cudaStreamCreateWithFlags scene build");
        try {
            fSlot = std::make_unique<TransportSlot>(fConfiguration.enablePerformanceDiagnostics);
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
        fSlot.reset();
        if (fPipeline != nullptr) {
            optixPipelineDestroy(fPipeline);
        }
        if (fHitgroupProgram != nullptr) {
            optixProgramGroupDestroy(fHitgroupProgram);
        }
        if (fMissProgram != nullptr) {
            optixProgramGroupDestroy(fMissProgram);
        }
        if (fRaygenProgram != nullptr) {
            optixProgramGroupDestroy(fRaygenProgram);
        }
        if (fModule != nullptr) {
            optixModuleDestroy(fModule);
        }
        fSbtAllocations.clear();
        fSceneAllocations.clear();
        if (fSceneBuildStream != nullptr) {
            cudaStreamDestroy(fSceneBuildStream);
            fSceneBuildStream = nullptr;
        }
        if (fContext != nullptr) {
            optixDeviceContextDestroy(fContext);
        }
    }

    auto PrepareScene(const Scene& scene) -> void {
        std::lock_guard lock{fSceneMutex};
        if (fSceneAddress != &scene) {
            if (fSlot->busy) {
                throw std::logic_error("OptiX scene cannot be rebuilt with pending batches");
            }
            BuildScene(scene);
        }
    }

    auto EnqueueEmissions(const Scene& scene, const PhotonTransportBatch& batch) -> void {
        PrepareScene(scene);
        if (fSlot->busy) {
            throw std::logic_error("OptiX transport queue is full");
        }
        auto& slot{*fSlot};
        auto& stream{slot.stream};
        auto& fEmissionAllocation{slot.emissionAllocation};
        auto& fEmissionOffsetAllocation{slot.emissionOffsetAllocation};
        auto& fEmissionEventIndexAllocation{slot.emissionEventIndexAllocation};
        auto& fHitAllocation{slot.hitAllocation};
        auto& fHitFlagAllocation{slot.hitFlagAllocation};
        auto& fCompactedHitAllocation{slot.compactedHitAllocation};
        auto& fHitCountAllocation{slot.hitCountAllocation};
        auto& fCompactionTemporaryAllocation{slot.compactionTemporaryAllocation};
        auto& fStatsAllocation{slot.statsAllocation};
        auto& fEventStatsAllocation{slot.eventStatsAllocation};
        auto& fLaunchParamsAllocation{slot.launchParamsAllocation};
        auto& fEmissionCapacity{slot.emissionCapacity};
        auto& fEmissionOffsetCapacity{slot.emissionOffsetCapacity};
        auto& fEmissionEventIndexCapacity{slot.emissionEventIndexCapacity};
        auto& fHitCapacity{slot.hitCapacity};
        auto& fHitFlagCapacity{slot.hitFlagCapacity};
        auto& fCompactedHitCapacity{slot.compactedHitCapacity};
        auto& fHitCountCapacity{slot.hitCountCapacity};
        auto& fCompactionTemporaryCapacity{slot.compactionTemporaryCapacity};
        auto& fCompactionInputCapacity{slot.compactionInputCapacity};
        auto& fStatsCapacity{slot.statsCapacity};
        auto& fEventStatsCapacity{slot.eventStatsCapacity};
        auto& fLaunchParamsCapacity{slot.launchParamsCapacity};
        auto& fHostEmissions{slot.hostEmissions};
        auto& fHostEmissionOffsets{slot.hostEmissionOffsets};
        auto& fHostEmissionEventIndices{slot.hostEmissionEventIndices};
        auto& fHostCompactHits{slot.hostCompactHits};
        auto& fHostHitCount{slot.hostHitCount};
        auto& fHostStats{slot.hostStats};
        auto& fHostEventStats{slot.hostEventStats};
        auto& fHostEmissionCapacity{slot.hostEmissionCapacity};
        auto& fHostEmissionOffsetCapacity{slot.hostEmissionOffsetCapacity};
        auto& fHostEmissionEventIndexCapacity{slot.hostEmissionEventIndexCapacity};
        auto& fHostCompactHitCapacity{slot.hostCompactHitCapacity};
        auto& fHostHitCountCapacity{slot.hostHitCountCapacity};
        auto& fHostStatsCapacity{slot.hostStatsCapacity};
        auto& fHostEventStatsCapacity{slot.hostEventStatsCapacity};
        auto& fCompactionReadyEvent{slot.compactionReadyEvent};
        auto& fHostToDeviceTimer{slot.hostToDeviceTimer};
        auto& fDeviceMemsetTimer{slot.deviceMemsetTimer};
        auto& fOptiXKernelTimer{slot.optiXKernelTimer};
        auto& fDeviceHitCompactionTimer{slot.deviceHitCompactionTimer};
        auto& fMetadataToHostTimer{slot.metadataToHostTimer};

        PhotonTransportOutput output{};
        const auto emissions{batch.emissions};
        if (batch.eventIDs.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("OptiX event batch exceeds the supported event count");
        }
        if (batch.emissionEventIndices.size() != emissions.size()) {
            throw std::invalid_argument("OptiX emission event-index count does not match emissions");
        }
        std::unordered_map<std::uint32_t, std::uint32_t> eventIndices{};
        eventIndices.reserve(batch.eventIDs.size());
        for (auto index{std::size_t{}}; index < batch.eventIDs.size(); ++index) {
            if (!eventIndices.emplace(batch.eventIDs[index], static_cast<std::uint32_t>(index)).second) {
                throw std::invalid_argument("OptiX event batch contains duplicate event IDs");
            }
            output.eventStatistics.emplace_back(PhotonTransportEventStatistics{batch.eventIDs[index], {}});
        }
        for (auto index{std::size_t{}}; index < emissions.size(); ++index) {
            const auto eventIndex{batch.emissionEventIndices[index]};
            if (eventIndex >= batch.eventIDs.size() || emissions[index].eventID != batch.eventIDs[eventIndex]) {
                throw std::invalid_argument("OptiX emission event index does not match event ID");
            }
            auto& eventStatistics{output.eventStatistics.at(eventIndex).statistics};
            eventStatistics.capturedCount += emissions[index].photonCount;
            eventStatistics.validFields |= StatisticFieldBit(PhotonTransportStatisticField::Captured);
        }
        if (emissions.empty()) {
            slot.output = std::move(output);
            slot.photonCount = 0;
            slot.startedAt = std::chrono::steady_clock::now();
            slot.busy = true;
            return;
        }
        if (emissions.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("OptiX emission batch exceeds the supported launch size");
        }

        std::uint64_t photonCount{};
        for (const auto& emission : emissions) {
            photonCount += emission.photonCount;
        }
        if (photonCount == 0) {
            for (auto& eventOutput : output.eventStatistics) {
                eventOutput.statistics.validFields |= StatisticFieldBit(PhotonTransportStatisticField::Detected) |
                                                      StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
                                                      StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
                                                      StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
                                                      StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
                                                      StatisticFieldBit(PhotonTransportStatisticField::InvalidState) |
                                                      StatisticFieldBit(PhotonTransportStatisticField::ZeroStep);
            }
            slot.output = std::move(output);
            slot.photonCount = 0;
            slot.startedAt = std::chrono::steady_clock::now();
            slot.busy = true;
            return;
        }
        if (photonCount > std::numeric_limits<std::uint32_t>::max()) {
            throw std::overflow_error("OptiX emission batch exceeds the supported photon count");
        }
        output.statistics.capturedCount = photonCount;
        output.statistics.validFields = StatisticFieldBit(PhotonTransportStatisticField::Captured);
        slot.hostToDeviceBytes = emissions.size() * sizeof(DeviceOpticalEmission) +
                                 (emissions.size() + 1U) * sizeof(std::uint32_t) +
                                 emissions.size() * sizeof(std::uint32_t) + sizeof(OptixLaunchParams);

        const auto start{std::chrono::steady_clock::now()};
        EnsurePinnedAllocation(fHostEmissions, fHostEmissionCapacity, emissions.size(), "cudaMallocHost emissions");
        EnsurePinnedAllocation(fHostEmissionOffsets, fHostEmissionOffsetCapacity, emissions.size() + 1U,
                               "cudaMallocHost emission offsets");
        EnsurePinnedAllocation(fHostEmissionEventIndices, fHostEmissionEventIndexCapacity, emissions.size(),
                               "cudaMallocHost emission event indices");
        fHostEmissionOffsets[0] = 0;
        std::uint64_t offset{};
        for (auto index{std::size_t{}}; index < emissions.size(); ++index) {
            const auto& emission{emissions[index]};
            auto deviceEmission{std::bit_cast<DeviceOpticalEmission>(emission)};
            auto deviceVolumeID{InvalidID};
            if (emission.volumeID < fVolumeIndicesByID.size()) {
                deviceVolumeID = fVolumeIndicesByID.at(emission.volumeID);
            } else if (const auto volumeIndex{fVolumeIndices.find(emission.volumeID)};
                       volumeIndex != fVolumeIndices.end()) {
                deviceVolumeID = volumeIndex->second;
            }
            if (deviceVolumeID == InvalidID) {
                throw std::invalid_argument("emission references an unknown scene volume ID");
            }
            if (emission.materialID >= scene.Materials().size()) {
                throw std::invalid_argument("emission references an unknown scene material ID");
            }
            deviceEmission.volumeID = deviceVolumeID;
            fHostEmissions[index] = deviceEmission;
            fHostEmissionEventIndices[index] = batch.emissionEventIndices[index];
            offset += emission.photonCount;
            fHostEmissionOffsets[index + 1U] = static_cast<std::uint32_t>(offset);
        }

        EnsureAllocation(fEmissionAllocation, fEmissionCapacity, emissions.size(), sizeof(DeviceOpticalEmission));
        EnsureAllocation(fEmissionOffsetAllocation, fEmissionOffsetCapacity, emissions.size() + 1U,
                         sizeof(std::uint32_t));
        EnsureAllocation(fEmissionEventIndexAllocation, fEmissionEventIndexCapacity, emissions.size(),
                         sizeof(std::uint32_t));
        EnsureAllocation(fHitAllocation, fHitCapacity, static_cast<std::size_t>(photonCount), sizeof(DevicePhotonHit));
        EnsureAllocation(fHitFlagAllocation, fHitFlagCapacity, static_cast<std::size_t>(photonCount),
                         sizeof(std::uint32_t));
        EnsureAllocation(fCompactedHitAllocation, fCompactedHitCapacity, static_cast<std::size_t>(photonCount),
                         sizeof(DevicePhotonHit));
        EnsureAllocation(fHitCountAllocation, fHitCountCapacity, 1, sizeof(std::uint32_t));
        EnsureAllocation(fStatsAllocation, fStatsCapacity, 1, sizeof(DeviceTransportStats));
        EnsureAllocation(fEventStatsAllocation, fEventStatsCapacity, batch.eventIDs.size(),
                         sizeof(DeviceEventTransportStats));
        EnsureAllocation(fLaunchParamsAllocation, fLaunchParamsCapacity, 1, sizeof(OptixLaunchParams));
        EnsurePinnedAllocation(fHostCompactHits, fHostCompactHitCapacity, static_cast<std::size_t>(photonCount),
                               "cudaMallocHost compact hits");
        EnsurePinnedAllocation(fHostHitCount, fHostHitCountCapacity, 1, "cudaMallocHost hit count");
        EnsurePinnedAllocation(fHostStats, fHostStatsCapacity, 1, "cudaMallocHost transport stats");
        EnsurePinnedAllocation(fHostEventStats, fHostEventStatsCapacity, batch.eventIDs.size(),
                               "cudaMallocHost event transport stats");

        if (fCompactionInputCapacity < static_cast<std::size_t>(photonCount)) {
            const auto compactionBytes{QueryOptiXHitCompactionBytes(static_cast<std::size_t>(photonCount))};
            if (compactionBytes == 0) {
                throw std::runtime_error("unable to determine OptiX hit compaction temporary "
                                         "storage");
            }
            EnsureAllocation(fCompactionTemporaryAllocation, fCompactionTemporaryCapacity, compactionBytes, 1);
            fCompactionInputCapacity = static_cast<std::size_t>(photonCount);
        }

        RecordCudaTimerStart(fHostToDeviceTimer, stream);
        CudaError(cudaMemcpyAsync(DevicePointer(fEmissionAllocation->Pointer()), fHostEmissions,
                                  emissions.size() * sizeof(DeviceOpticalEmission), cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync emissions");
        CudaError(cudaMemcpyAsync(DevicePointer(fEmissionOffsetAllocation->Pointer()), fHostEmissionOffsets,
                                  (emissions.size() + 1U) * sizeof(std::uint32_t), cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync emission offsets");
        CudaError(cudaMemcpyAsync(DevicePointer(fEmissionEventIndexAllocation->Pointer()), fHostEmissionEventIndices,
                                  emissions.size() * sizeof(std::uint32_t), cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync emission event indices");

        OptixLaunchParams launchParams{};
        launchParams.emissions = DevicePointerAs<DeviceOpticalEmission>(fEmissionAllocation->Pointer());
        launchParams.emissionEventIndices =
            DevicePointerAs<const std::uint32_t>(fEmissionEventIndexAllocation->Pointer());
        launchParams.emissionOffsets = DevicePointerAs<const std::uint32_t>(fEmissionOffsetAllocation->Pointer());
        launchParams.hits = DevicePointerAs<DevicePhotonHit>(fHitAllocation->Pointer());
        launchParams.hitFlags = DevicePointerAs<std::uint32_t>(fHitFlagAllocation->Pointer());
        launchParams.stats = DevicePointerAs<DeviceTransportStats>(fStatsAllocation->Pointer());
        launchParams.eventStats = DevicePointerAs<DeviceEventTransportStats>(fEventStatsAllocation->Pointer());
        launchParams.scene = fDeviceScene;
        launchParams.traversable = fTraversableHandle;
        launchParams.seed = fConfiguration.seed;
        launchParams.maxBounceCount = fConfiguration.maxBouncesPerPhoton;
        launchParams.emissionCount = static_cast<std::uint32_t>(emissions.size());
        launchParams.eventCount = static_cast<std::uint32_t>(batch.eventIDs.size());
        launchParams.photonCount = static_cast<std::uint32_t>(photonCount);
        launchParams.enablePerformanceDiagnostics = fConfiguration.enablePerformanceDiagnostics ? 1U : 0U;
        launchParams.boundaryEpsilonMm = fConfiguration.boundaryToleranceMm;
        slot.hostLaunchParams = launchParams;
        CudaError(cudaMemcpyAsync(DevicePointer(fLaunchParamsAllocation->Pointer()), &slot.hostLaunchParams,
                                  sizeof(slot.hostLaunchParams), cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync launch params");
        RecordCudaTimerStop(fHostToDeviceTimer, stream);

        RecordCudaTimerStart(fDeviceMemsetTimer, stream);
        CudaError(cudaMemsetAsync(DevicePointer(fStatsAllocation->Pointer()), 0, sizeof(DeviceTransportStats), stream),
                  "cudaMemsetAsync transport stats");
        CudaError(cudaMemsetAsync(DevicePointer(fEventStatsAllocation->Pointer()), 0,
                                  batch.eventIDs.size() * sizeof(DeviceEventTransportStats), stream),
                  "cudaMemsetAsync event transport stats");
        CudaError(cudaMemsetAsync(DevicePointer(fHitFlagAllocation->Pointer()), 0,
                                  static_cast<std::size_t>(photonCount) * sizeof(std::uint32_t), stream),
                  "cudaMemsetAsync hit flags");
        RecordCudaTimerStop(fDeviceMemsetTimer, stream);

        RecordCudaTimerStart(fOptiXKernelTimer, stream);
        OptiXError(optixLaunch(fPipeline, stream, fLaunchParamsAllocation->Pointer(), sizeof(launchParams), &fSbt,
                               static_cast<unsigned int>(photonCount), 1, 1),
                   "optixLaunch");
        RecordCudaTimerStop(fOptiXKernelTimer, stream);
        RecordCudaTimerStart(fDeviceHitCompactionTimer, stream);
        CudaError(CompactOptiXHits(fHitAllocation->Pointer(), fHitFlagAllocation->Pointer(),
                                   fCompactedHitAllocation->Pointer(), fHitCountAllocation->Pointer(),
                                   fCompactionTemporaryAllocation->Pointer(), fCompactionTemporaryCapacity,
                                   static_cast<std::size_t>(photonCount), stream),
                  "OptiX hit compaction");
        RecordCudaTimerStop(fDeviceHitCompactionTimer, stream);

        RecordCudaTimerStart(fMetadataToHostTimer, stream);
        CudaError(cudaMemcpyAsync(fHostStats, DevicePointer(fStatsAllocation->Pointer()), sizeof(DeviceTransportStats),
                                  cudaMemcpyDeviceToHost, stream),
                  "cudaMemcpyAsync transport stats");
        CudaError(cudaMemcpyAsync(fHostEventStats, DevicePointer(fEventStatsAllocation->Pointer()),
                                  batch.eventIDs.size() * sizeof(DeviceEventTransportStats), cudaMemcpyDeviceToHost,
                                  stream),
                  "cudaMemcpyAsync event transport stats");
        CudaError(cudaMemcpyAsync(fHostHitCount, DevicePointer(fHitCountAllocation->Pointer()), sizeof(std::uint32_t),
                                  cudaMemcpyDeviceToHost, stream),
                  "cudaMemcpyAsync hit count");
        RecordCudaTimerStop(fMetadataToHostTimer, stream);
        CudaError(cudaEventRecord(fCompactionReadyEvent, stream), "cudaEventRecord compaction ready");

        slot.output = std::move(output);
        slot.photonCount = static_cast<std::size_t>(photonCount);
        slot.startedAt = start;
        slot.busy = true;
    }

    auto CompleteOldestBatch() -> PhotonTransportOutput {
        if (!fSlot->busy) {
            throw std::logic_error("OptiX transport queue is empty");
        }
        auto& slot{*fSlot};
        auto& stream{slot.stream};
        auto& output{slot.output};
        const auto photonCount{slot.photonCount};

        if (photonCount == 0) {
            auto completedOutput{std::move(output)};
            slot.output = {};
            slot.busy = false;
            return completedOutput;
        }

        CudaError(cudaEventSynchronize(slot.compactionReadyEvent), "cudaEventSynchronize compaction ready");
        const auto compactHitCount{static_cast<std::size_t>(*slot.hostHitCount)};
        if (compactHitCount > photonCount) {
            throw std::runtime_error("OptiX hit compaction returned an invalid hit count");
        }
        RecordCudaTimerStart(slot.hitsToHostTimer, stream);
        if (compactHitCount > 0) {
            CudaError(cudaMemcpyAsync(slot.hostCompactHits, DevicePointer(slot.compactedHitAllocation->Pointer()),
                                      compactHitCount * sizeof(DevicePhotonHit), cudaMemcpyDeviceToHost, stream),
                      "cudaMemcpyAsync compact hits");
        }
        RecordCudaTimerStop(slot.hitsToHostTimer, stream);
        CudaError(cudaStreamSynchronize(stream), "cudaStreamSynchronize");

        const auto& deviceStats{*slot.hostStats};
        output.statistics.detectedCount = deviceStats.detectedCount;
        output.statistics.absorbedCount = deviceStats.absorbedCount;
        output.statistics.escapedCount = deviceStats.escapedCount;
        output.statistics.truncatedCount = deviceStats.truncatedCount;
        output.statistics.maxBounceCount = deviceStats.maxBounceCount;
        output.statistics.invalidStateCount = deviceStats.invalidStateCount;
        output.statistics.zeroStepCount = deviceStats.zeroStepCount;
        output.statistics.transportTimeMs =
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - slot.startedAt}.count();
        output.statistics.validFields |= StatisticFieldBit(PhotonTransportStatisticField::Detected) |
                                         StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
                                         StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
                                         StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
                                         StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
                                         StatisticFieldBit(PhotonTransportStatisticField::InvalidState) |
                                         StatisticFieldBit(PhotonTransportStatisticField::ZeroStep) |
                                         StatisticFieldBit(PhotonTransportStatisticField::TransportTime);
        for (auto index{std::size_t{}}; index < output.eventStatistics.size(); ++index) {
            auto& eventStatistics{output.eventStatistics.at(index).statistics};
            const auto& deviceEventStatistics{slot.hostEventStats[index]};
            eventStatistics.detectedCount = deviceEventStatistics.detectedCount;
            eventStatistics.absorbedCount = deviceEventStatistics.absorbedCount;
            eventStatistics.escapedCount = deviceEventStatistics.escapedCount;
            eventStatistics.truncatedCount = deviceEventStatistics.truncatedCount;
            eventStatistics.maxBounceCount = deviceEventStatistics.maxBounceCount;
            eventStatistics.invalidStateCount = deviceEventStatistics.invalidStateCount;
            eventStatistics.zeroStepCount = deviceEventStatistics.zeroStepCount;
            eventStatistics.validFields |= StatisticFieldBit(PhotonTransportStatisticField::Detected) |
                                           StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
                                           StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
                                           StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
                                           StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
                                           StatisticFieldBit(PhotonTransportStatisticField::InvalidState) |
                                           StatisticFieldBit(PhotonTransportStatisticField::ZeroStep);
        }
        output.performance.hostToDeviceMs = ReadCudaTimerMs(slot.hostToDeviceTimer);
        output.performance.hostToDeviceBytes = slot.hostToDeviceBytes;
        output.performance.deviceMemsetMs = ReadCudaTimerMs(slot.deviceMemsetTimer);
        output.performance.optiXKernelMs = ReadCudaTimerMs(slot.optiXKernelTimer);
        output.performance.deviceHitCompactionMs = ReadCudaTimerMs(slot.deviceHitCompactionTimer);
        output.performance.deviceMetadataToHostMs = ReadCudaTimerMs(slot.metadataToHostTimer);
        output.performance.deviceMetadataToHostBytes =
            sizeof(DeviceTransportStats) + sizeof(std::uint32_t) +
            output.eventStatistics.size() * sizeof(DeviceEventTransportStats);
        output.performance.deviceHitsToHostMs = ReadCudaTimerMs(slot.hitsToHostTimer);
        output.performance.deviceHitsToHostBytes = compactHitCount * sizeof(DevicePhotonHit);
        output.performance.deviceToHostMs =
            output.performance.deviceMetadataToHostMs + output.performance.deviceHitsToHostMs;
        output.performance.totalBounceCount = deviceStats.totalBounceCount;
        output.performance.coincidentCandidateTraceCount = deviceStats.coincidentCandidateTraceCount;
        output.performance.coincidentCandidateHitCount = deviceStats.coincidentCandidateHitCount;
        const auto hostCompactionStart{std::chrono::steady_clock::now()};
        output.detections.reserve(compactHitCount);
        for (auto index{std::size_t{}}; index < compactHitCount; ++index) {
            const auto& hit{slot.hostCompactHits[index]};
            output.detections.emplace_back(PhotonDetection{
                {hit.positionMm.at(0), hit.positionMm.at(1), hit.positionMm.at(2)},
                hit.timeNs,
                {hit.direction.at(0),  hit.direction.at(1),  hit.direction.at(2) },
                hit.energyEv,
                hit.eventID,
                hit.photonID,
                hit.sensorID,
            });
        }
        output.performance.hostHitCompactionMs =
            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - hostCompactionStart}.count();
        auto completedOutput{std::move(output)};
        slot.output = {};
        slot.photonCount = 0;
        slot.hostToDeviceBytes = 0;
        slot.busy = false;
        return completedOutput;
    }

private:
    auto CreatePipeline() -> void {
        OptixModuleCompileOptions moduleOptions{};
        moduleOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
        moduleOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
        moduleOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;

        OptixPipelineCompileOptions pipelineOptions{};
        pipelineOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
        pipelineOptions.numPayloadValues = 6;
        pipelineOptions.numAttributeValues = 2;
        pipelineOptions.pipelineLaunchParamsVariableName = "gLaunchParams";
        pipelineOptions.pipelineLaunchParamsSizeInBytes = sizeof(OptixLaunchParams);
        pipelineOptions.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;

        std::array<char, 8192> log{};
        auto logSize{log.size()};
        const auto moduleStatus{
            optixModuleCreate(fContext, &moduleOptions, &pipelineOptions, reinterpret_cast<const char*>(g4go_optix_ir),
                              static_cast<std::size_t>(g4go_optix_irLength), log.data(), &logSize, &fModule)};
        if (moduleStatus != OPTIX_SUCCESS) {
            throw std::runtime_error(std::string("optixModuleCreate failed: ") + optixGetErrorName(moduleStatus) +
                                     " (" + optixGetErrorString(moduleStatus) + ") " +
                                     std::string(log.data(), logSize));
        }

        OptixProgramGroupOptions programOptions{};
        OptixProgramGroupDesc raygenDescription{};
        raygenDescription.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        raygenDescription.raygen.module = fModule;
        raygenDescription.raygen.entryFunctionName = "__raygen__rg";
        fRaygenProgram = CreateProgramGroup(raygenDescription, programOptions, "raygen program group");

        OptixProgramGroupDesc missDescription{};
        missDescription.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        missDescription.miss.module = fModule;
        missDescription.miss.entryFunctionName = "__miss__ms";
        fMissProgram = CreateProgramGroup(missDescription, programOptions, "miss program group");

        OptixProgramGroupDesc hitgroupDescription{};
        hitgroupDescription.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        hitgroupDescription.hitgroup.moduleCH = fModule;
        hitgroupDescription.hitgroup.entryFunctionNameCH = "__closesthit__ch";
        hitgroupDescription.hitgroup.moduleAH = fModule;
        hitgroupDescription.hitgroup.entryFunctionNameAH = "__anyhit__ah";
        fHitgroupProgram = CreateProgramGroup(hitgroupDescription, programOptions, "hitgroup program group");

        const std::array programGroups{
            fRaygenProgram,
            fMissProgram,
            fHitgroupProgram,
        };
        OptixPipelineLinkOptions linkOptions{};
        linkOptions.maxTraceDepth = 1;
        linkOptions.maxTraversableGraphDepth = 2;
        log.fill(0);
        logSize = log.size();
        const auto pipelineStatus{optixPipelineCreate(fContext, &pipelineOptions, &linkOptions, programGroups.data(),
                                                      static_cast<unsigned int>(programGroups.size()), log.data(),
                                                      &logSize, &fPipeline)};
        if (pipelineStatus != OPTIX_SUCCESS) {
            throw std::runtime_error(std::string("optixPipelineCreate failed: ") + optixGetErrorName(pipelineStatus) +
                                     " (" + optixGetErrorString(pipelineStatus) + ") " +
                                     std::string(log.data(), logSize));
        }
        OptiXError(optixPipelineSetStackSizeFromCallDepths(fPipeline, 1, 0, 0, 0, 2),
                   "optixPipelineSetStackSizeFromCallDepths");
    }

    auto CreateProgramGroup(const OptixProgramGroupDesc& description, const OptixProgramGroupOptions& options,
                            const char* operation) -> OptixProgramGroup {
        OptixProgramGroup programGroup{};
        std::array<char, 4096> log{};
        auto logSize{log.size()};
        const auto status{
            optixProgramGroupCreate(fContext, &description, 1, &options, log.data(), &logSize, &programGroup)};
        if (status != OPTIX_SUCCESS) {
            throw std::runtime_error(std::string(operation) + " failed: " + optixGetErrorName(status) + " (" +
                                     optixGetErrorString(status) + ") " + std::string(log.data(), logSize));
        }
        return programGroup;
    }

    auto CreateSbt() -> void {
        EmptySbtRecord raygenRecord{};
        EmptySbtRecord missRecord{};
        EmptySbtRecord hitgroupRecord{};
        OptiXError(optixSbtRecordPackHeader(fRaygenProgram, &raygenRecord), "optixSbtRecordPackHeader raygen");
        OptiXError(optixSbtRecordPackHeader(fMissProgram, &missRecord), "optixSbtRecordPackHeader miss");
        OptiXError(optixSbtRecordPackHeader(fHitgroupProgram, &hitgroupRecord), "optixSbtRecordPackHeader hitgroup");

        fSbt = {};
        fSbt.raygenRecord = UploadObject(raygenRecord, fSbtAllocations);
        fSbt.missRecordBase = UploadObject(missRecord, fSbtAllocations);
        fSbt.missRecordStrideInBytes = sizeof(EmptySbtRecord);
        fSbt.missRecordCount = 1;
        fSbt.hitgroupRecordBase = UploadObject(hitgroupRecord, fSbtAllocations);
        fSbt.hitgroupRecordStrideInBytes = sizeof(EmptySbtRecord);
        fSbt.hitgroupRecordCount = 1;
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
            const auto volumeID{scene.Volumes().at(index).volumeID};
            if (volumeID == InvalidID || !volumeIndices.emplace(volumeID, static_cast<std::uint32_t>(index)).second) {
                throw std::invalid_argument("OptiX scene volume IDs must be unique and valid");
            }
        }
        const auto mapVolumeID{[&](std::uint32_t volumeID, const char* relationship) {
            if (volumeID == InvalidID) {
                return InvalidID;
            }
            const auto volumeIndex{volumeIndices.find(volumeID)};
            if (volumeIndex == volumeIndices.end()) {
                throw std::invalid_argument(std::string("OptiX scene references an unknown ") + relationship +
                                            " volume ID");
            }
            return volumeIndex->second;
        }};
        const auto mapRequiredVolumeID{[&](std::uint32_t volumeID, const char* relationship) {
            if (volumeID == InvalidID) {
                throw std::invalid_argument(std::string("OptiX scene has no ") + relationship + " volume ID");
            }
            return mapVolumeID(volumeID, relationship);
        }};
        fSceneAllocations.clear();

        std::vector<DeviceMaterial> materials{};
        materials.reserve(scene.Materials().size());
        for (const auto& material : scene.Materials()) {
            materials.emplace_back(DeviceMaterial{
                UploadProperty(material.rindex, fSceneAllocations),
                material.rindexMax,
                UploadProperty(material.groupVelocityMmPerNs, fSceneAllocations),
                UploadProperty(material.absLengthMm, fSceneAllocations),
                {
                                                           UploadSpectrumProperty(material.scintillationSpectrum.at(0), fSceneAllocations),
                                                           UploadSpectrumProperty(material.scintillationSpectrum.at(1), fSceneAllocations),
                                                           UploadSpectrumProperty(material.scintillationSpectrum.at(2), fSceneAllocations),
                                                           },
            });
        }

        std::vector<DeviceSurface> surfaces{};
        surfaces.reserve(scene.Surfaces().size());
        for (const auto& surface : scene.Surfaces()) {
            surfaces.emplace_back(DeviceSurface{
                static_cast<std::uint8_t>(surface.type),
                static_cast<std::uint8_t>(surface.model),
                static_cast<std::uint8_t>(surface.finish),
                0,
                surface.modelValue,
                UploadProperty(surface.reflectivity, fSceneAllocations),
                UploadProperty(surface.efficiency, fSceneAllocations),
                UploadProperty(surface.transmittance, fSceneAllocations),
                UploadProperty(surface.rindex, fSceneAllocations),
                UploadProperty(surface.specularLobe, fSceneAllocations),
                UploadProperty(surface.specularSpike, fSceneAllocations),
                UploadProperty(surface.backscatter, fSceneAllocations),
                UploadProperty(surface.surfaceRoughness, fSceneAllocations),
            });
        }

        std::vector<DeviceGeometry> geometries{};
        geometries.reserve(scene.Geometries().size());
        std::vector<OptixTraversableHandle> geometryHandles{};
        geometryHandles.reserve(scene.Geometries().size());
        for (const auto& geometry : scene.Geometries()) {
            const auto& mesh{geometry.mesh};
            if (mesh.verticesMm.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
                throw std::invalid_argument("OptiX scene contains invalid mesh " + geometry.name);
            }
            if (std::any_of(mesh.indices.begin(), mesh.indices.end(),
                            [&](const auto index) { return index >= mesh.verticesMm.size(); })) {
                throw std::invalid_argument("OptiX scene mesh index exceeds vertex count: " + geometry.name);
            }
            if (!mesh.triangleFlags.empty() && mesh.triangleFlags.size() != mesh.indices.size() / 3) {
                throw std::invalid_argument("OptiX scene triangle flag count mismatch: " + geometry.name);
            }
            const auto triangleCount{mesh.indices.size() / 3};
            std::vector<G4ThreeVector> triangleNormals{};
            if (mesh.triangleNormals.empty()) {
                triangleNormals.reserve(triangleCount);
                for (auto triangle{std::size_t{}}; triangle < triangleCount; ++triangle) {
                    const auto base{3 * triangle};
                    const auto& first{mesh.verticesMm.at(mesh.indices.at(base))};
                    const auto& second{mesh.verticesMm.at(mesh.indices.at(base + 1))};
                    const auto& third{mesh.verticesMm.at(mesh.indices.at(base + 2))};
                    const auto normal{(second - first).cross(third - first)};
                    if (!(normal.mag2() > 0.0)) {
                        throw std::invalid_argument("OptiX scene contains a degenerate triangle: " + geometry.name);
                    }
                    triangleNormals.emplace_back(normal.unit());
                }
            } else {
                if (mesh.triangleNormals.size() != triangleCount) {
                    throw std::invalid_argument("OptiX scene triangle normal count mismatch: " + geometry.name);
                }
                triangleNormals.reserve(triangleCount);
                for (const auto& normal : mesh.triangleNormals) {
                    if (!(normal.mag2() > 0.0)) {
                        throw std::invalid_argument("OptiX scene contains an invalid triangle normal: " +
                                                    geometry.name);
                    }
                    triangleNormals.emplace_back(normal.unit());
                }
            }
            std::vector<DeviceVector3> vertices{};
            vertices.reserve(mesh.verticesMm.size());
            for (const auto& vertex : mesh.verticesMm) {
                vertices.emplace_back(ToDevice(vertex));
            }
            const auto vertexPointer{Upload<DeviceVector3>(vertices, fSceneAllocations)};
            const auto indexPointer{Upload<std::uint32_t>(mesh.indices, fSceneAllocations)};
            std::vector<DeviceVector3> deviceNormals{};
            deviceNormals.reserve(triangleNormals.size());
            for (const auto& normal : triangleNormals) {
                deviceNormals.emplace_back(ToDevice(normal));
            }
            const auto normalPointer{Upload<DeviceVector3>(deviceNormals, fSceneAllocations)};
            const auto flagPointer{Upload<std::uint8_t>(mesh.triangleFlags, fSceneAllocations)};
            geometries.emplace_back(DeviceGeometry{
                {
                 DevicePointerAs<const DeviceVector3>(vertexPointer),
                 DevicePointerAs<const std::uint32_t>(indexPointer),
                 DevicePointerAs<const DeviceVector3>(normalPointer),
                 DevicePointerAs<const std::uint8_t>(flagPointer),
                 static_cast<std::uint32_t>(mesh.verticesMm.size()),
                 static_cast<std::uint32_t>(triangleCount),
                 },
            });

            const CUdeviceptr vertexBuffers[]{vertexPointer};
            const unsigned int geometryFlags[]{OPTIX_GEOMETRY_FLAG_NONE};
            OptixBuildInput buildInput{};
            buildInput.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
            buildInput.triangleArray.vertexBuffers = vertexBuffers;
            buildInput.triangleArray.numVertices = static_cast<unsigned int>(mesh.verticesMm.size());
            buildInput.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
            buildInput.triangleArray.indexBuffer = indexPointer;
            buildInput.triangleArray.numIndexTriplets = static_cast<unsigned int>(mesh.indices.size() / 3);
            buildInput.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
            buildInput.triangleArray.flags = geometryFlags;
            buildInput.triangleArray.numSbtRecords = 1;

            OptixAccelBuildOptions buildOptions{};
            buildOptions.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
            buildOptions.operation = OPTIX_BUILD_OPERATION_BUILD;
            buildOptions.motionOptions.numKeys = 1;
            OptixAccelBufferSizes bufferSizes{};
            OptiXError(optixAccelComputeMemoryUsage(fContext, &buildOptions, &buildInput, 1, &bufferSizes),
                       "optixAccelComputeMemoryUsage geometry");
            auto temporary{std::make_unique<DeviceAllocation>(bufferSizes.tempSizeInBytes)};
            auto output{std::make_unique<DeviceAllocation>(bufferSizes.outputSizeInBytes)};
            OptixTraversableHandle geometryHandle{};
            OptiXError(optixAccelBuild(fContext, fSceneBuildStream, &buildOptions, &buildInput, 1, temporary->Pointer(),
                                       bufferSizes.tempSizeInBytes, output->Pointer(), bufferSizes.outputSizeInBytes,
                                       &geometryHandle, nullptr, 0),
                       "optixAccelBuild geometry");
            CudaError(cudaStreamSynchronize(fSceneBuildStream), "cudaStreamSynchronize geometry GAS");
            fSceneAllocations.emplace_back(std::move(temporary));
            fSceneAllocations.emplace_back(std::move(output));
            geometryHandles.emplace_back(geometryHandle);
        }

        std::vector<DeviceVolume> volumes{};
        volumes.reserve(scene.Volumes().size());
        std::vector<OptixInstance> instances{};
        instances.reserve(scene.Volumes().size());
        for (auto index{std::size_t{}}; index < scene.Volumes().size(); ++index) {
            const auto& volume{scene.Volumes().at(index)};
            if (volume.geometryID >= geometryHandles.size()) {
                throw std::invalid_argument("volume references invalid geometry");
            }
            volumes.emplace_back(DeviceVolume{
                static_cast<std::uint32_t>(index),
                volume.physicalVolumeID,
                volume.copyNo,
                volume.geometryID,
                volume.materialID,
                mapVolumeID(volume.parentVolumeID, "parent"),
                volume.skinSurfaceID,
                volume.sensorID,
                volume.depth,
                static_cast<std::uint8_t>(volume.mayHaveCoincidentBoundary),
                {},
            });
            const auto& rotation{volume.transform.rotation};
            const auto& translation{volume.transform.translationMm};
            OptixInstance instance{};
            instance.transform[0] = rotation.xx;
            instance.transform[1] = rotation.xy;
            instance.transform[2] = rotation.xz;
            instance.transform[3] = static_cast<float>(translation.x());
            instance.transform[4] = rotation.yx;
            instance.transform[5] = rotation.yy;
            instance.transform[6] = rotation.yz;
            instance.transform[7] = static_cast<float>(translation.y());
            instance.transform[8] = rotation.zx;
            instance.transform[9] = rotation.zy;
            instance.transform[10] = rotation.zz;
            instance.transform[11] = static_cast<float>(translation.z());
            instance.instanceId = static_cast<unsigned int>(index);
            instance.visibilityMask = 255;
            instance.flags = OPTIX_INSTANCE_FLAG_DISABLE_TRIANGLE_FACE_CULLING;
            instance.traversableHandle = geometryHandles.at(volume.geometryID);
            instances.emplace_back(instance);
        }

        std::vector<DeviceSurfaceBinding> deviceBindings{};
        deviceBindings.reserve(scene.SurfaceBindings().size());
        for (const auto& binding : scene.SurfaceBindings()) {
            deviceBindings.emplace_back(DeviceSurfaceBinding{
                mapRequiredVolumeID(binding.fromVolumeID, "boundary source"),
                mapRequiredVolumeID(binding.toVolumeID, "boundary target"),
                binding.surfaceID,
            });
        }
        std::sort(deviceBindings.begin(), deviceBindings.end(), [](const auto& left, const auto& right) {
            if (left.fromVolumeID != right.fromVolumeID) {
                return left.fromVolumeID < right.fromVolumeID;
            }
            return left.toVolumeID < right.toVolumeID;
        });

        const auto materialPointer{Upload<DeviceMaterial>(materials, fSceneAllocations)};
        const auto surfacePointer{Upload<DeviceSurface>(surfaces, fSceneAllocations)};
        const auto geometryPointer{Upload<DeviceGeometry>(geometries, fSceneAllocations)};
        const auto volumePointer{Upload<DeviceVolume>(volumes, fSceneAllocations)};
        const auto bindingPointer{Upload<DeviceSurfaceBinding>(deviceBindings, fSceneAllocations)};
        const auto instancePointer{Upload<OptixInstance>(instances, fSceneAllocations)};

        fDeviceScene = {
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
        instanceInput.instanceArray.numInstances = static_cast<unsigned int>(instances.size());
        OptixAccelBuildOptions instanceOptions{};
        instanceOptions.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
        instanceOptions.operation = OPTIX_BUILD_OPERATION_BUILD;
        OptixAccelBufferSizes instanceBufferSizes{};
        OptiXError(optixAccelComputeMemoryUsage(fContext, &instanceOptions, &instanceInput, 1, &instanceBufferSizes),
                   "optixAccelComputeMemoryUsage IAS");
        auto instanceTemporary{std::make_unique<DeviceAllocation>(instanceBufferSizes.tempSizeInBytes)};
        auto instanceOutput{std::make_unique<DeviceAllocation>(instanceBufferSizes.outputSizeInBytes)};
        OptiXError(optixAccelBuild(fContext, fSceneBuildStream, &instanceOptions, &instanceInput, 1,
                                   instanceTemporary->Pointer(), instanceBufferSizes.tempSizeInBytes,
                                   instanceOutput->Pointer(), instanceBufferSizes.outputSizeInBytes,
                                   &fTraversableHandle, nullptr, 0),
                   "optixAccelBuild IAS");
        CudaError(cudaStreamSynchronize(fSceneBuildStream), "cudaStreamSynchronize scene IAS");
        fSceneAllocations.emplace_back(std::move(instanceTemporary));
        fSceneAllocations.emplace_back(std::move(instanceOutput));
        if (std::all_of(volumeIndices.begin(), volumeIndices.end(),
                        [&](const auto& entry) { return entry.first < scene.Volumes().size(); })) {
            fVolumeIndicesByID.assign(scene.Volumes().size(), InvalidID);
            for (const auto& [volumeID, volumeIndex] : volumeIndices) {
                fVolumeIndicesByID[volumeID] = volumeIndex;
            }
        } else {
            fVolumeIndicesByID.clear();
        }
        fVolumeIndices = std::move(volumeIndices);
        fSceneAddress = &scene;
    }

    PhotonTransportConfig fConfiguration;
    OptixDeviceContext fContext;
    OptixModule fModule;
    OptixProgramGroup fRaygenProgram;
    OptixProgramGroup fMissProgram;
    OptixProgramGroup fHitgroupProgram;
    OptixPipeline fPipeline;
    cudaStream_t fSceneBuildStream;
    std::unique_ptr<TransportSlot> fSlot;
    OptixShaderBindingTable fSbt;
    DeviceAllocations fSbtAllocations;
    DeviceAllocations fSceneAllocations;
    DeviceScene fDeviceScene;
    std::unordered_map<std::uint32_t, std::uint32_t> fVolumeIndices;
    std::vector<std::uint32_t> fVolumeIndicesByID;
    std::mutex fSceneMutex;
    const Scene* fSceneAddress;
    OptixTraversableHandle fTraversableHandle;
};

OptiXTransportHost::OptiXTransportHost(PhotonTransportConfig configuration) :
    fImpl{std::make_unique<Impl>(configuration)} {}

OptiXTransportHost::~OptiXTransportHost() = default;

auto OptiXTransportHost::PrepareScene(const Scene& scene) -> void { fImpl->PrepareScene(scene); }

auto OptiXTransportHost::EnqueueEmissions(const Scene& scene, const PhotonTransportBatch& batch) -> void {
    fImpl->EnqueueEmissions(scene, batch);
}

auto OptiXTransportHost::CompleteOldestBatch() -> PhotonTransportOutput { return fImpl->CompleteOldestBatch(); }

} // namespace G4GO::Optical
