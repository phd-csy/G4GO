#include "g4go/optical/optix/OptiXTransportHost.hpp"

#include "G4ThreeVector.hh"
#include "OptiXDeviceData.cuh"
#include "cuda_runtime_api.h"
#include "g4go_optix_ir.h"
#include "optix_function_table_definition.h"
#include "optix_stubs.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
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

auto OptiXError(OptixResult result, const char* operation) -> void {
    if (result == OPTIX_SUCCESS) {
        return;
    }
    throw std::runtime_error(
        std::string(operation) + " failed: " + optixGetErrorName(result) +
        " (" + optixGetErrorString(result) + ")");
}

class DeviceAllocation final {
public:
    explicit DeviceAllocation(std::size_t size) {
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
    CUdeviceptr pointer{};
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
    allocations.push_back(std::move(allocation));
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
    };
}

auto ToDevice(const std::array<float, 3>& vector) -> DeviceVector3 {
    return {vector[0], vector[1], vector[2]};
}

auto ToDevice(const G4ThreeVector& vector) -> DeviceVector3 {
    return {static_cast<float>(vector.x()), static_cast<float>(vector.y()),
            static_cast<float>(vector.z())};
}

auto ToDevice(const Photon& photon) -> DevicePhoton {
    return {
        ToDevice(photon.fPositionMm),
        photon.fTimeNs,
        ToDevice(photon.fDirection),
        photon.fEnergyEv,
        ToDevice(photon.fPolarization),
        photon.fWeight,
        photon.fEventID,
        photon.fPhotonID,
        photon.fVolumeID,
        static_cast<std::uint8_t>(photon.fSource),
        photon.fFlags,
        photon.fReserved,
    };
}

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
        fConfiguration{configuration} {
        CudaError(cudaFree(nullptr), "CUDA initialization");
        OptiXError(optixInit(), "optixInit");

        OptixDeviceContextOptions contextOptions{};
        contextOptions.logCallbackFunction = LogCallback;
        contextOptions.logCallbackLevel = 3;
        OptiXError(optixDeviceContextCreate(nullptr, &contextOptions,
                                             &context),
                   "optixDeviceContextCreate");
        CudaError(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
                  "cudaStreamCreateWithFlags");

        CreatePipeline();
        CreateSbt();
    }

    ~Impl() {
        if (stream != nullptr) {
            cudaStreamSynchronize(stream);
        }
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
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
        }
        if (context != nullptr) {
            optixDeviceContextDestroy(context);
        }
    }

    auto Propagate(const Scene& scene, std::span<const Photon> photons)
        -> PhotonTransportOutput {
        if (sceneAddress != &scene) {
            BuildScene(scene);
        }

        PhotonTransportOutput result{};
        result.fStatistics.fCapturedCount = photons.size();
        if (photons.empty()) {
            return result;
        }

        const auto start{std::chrono::steady_clock::now()};
        fDevicePhotons.clear();
        fDevicePhotons.reserve(photons.size());
        for (const auto& photon : photons) {
            fDevicePhotons.push_back(ToDevice(photon));
        }

        EnsureAllocation(fPhotonAllocation, fPhotonCapacity,
                         fDevicePhotons.size(), sizeof(DevicePhoton));
        EnsureAllocation(fHitAllocation, fHitCapacity, photons.size(),
                         sizeof(DevicePhotonHit));
        EnsureAllocation(fHitFlagAllocation, fHitFlagCapacity,
                         photons.size(), sizeof(std::uint32_t));
        EnsureAllocation(fStatsAllocation, fStatsCapacity, 1,
                         sizeof(DeviceTransportStats));
        EnsureAllocation(fLaunchParamsAllocation, fLaunchParamsCapacity, 1,
                         sizeof(OptixLaunchParams));
        fDeviceHits.resize(photons.size());
        fDeviceHitFlags.resize(photons.size());

        CudaError(cudaMemcpyAsync(DevicePointer(fPhotonAllocation->Pointer()),
                                  fDevicePhotons.data(),
                                  fDevicePhotons.size() * sizeof(DevicePhoton),
                                  cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync photons");
        CudaError(cudaMemsetAsync(DevicePointer(fStatsAllocation->Pointer()),
                                  0, sizeof(DeviceTransportStats), stream),
                  "cudaMemsetAsync transport stats");
        CudaError(cudaMemsetAsync(DevicePointer(fHitFlagAllocation->Pointer()),
                                  0, photons.size() * sizeof(std::uint32_t),
                                  stream),
                  "cudaMemsetAsync hit flags");

        OptixLaunchParams launchParams{};
        launchParams.fPhotons = DevicePointerAs<DevicePhoton>(
            fPhotonAllocation->Pointer());
        launchParams.fHits = DevicePointerAs<DevicePhotonHit>(
            fHitAllocation->Pointer());
        launchParams.fHitFlags = DevicePointerAs<std::uint32_t>(
            fHitFlagAllocation->Pointer());
        launchParams.fStats =
            DevicePointerAs<DeviceTransportStats>(fStatsAllocation->Pointer());
        launchParams.fScene = deviceScene;
        launchParams.fTraversable = traversableHandle;
        launchParams.fSeed = fConfiguration.fSeed;
        launchParams.fMaxBounceCount =
            fConfiguration.fMaxBouncesPerPhoton;
        launchParams.fPhotonCount = static_cast<std::uint32_t>(photons.size());
        launchParams.fBoundaryEpsilonMm =
            fConfiguration.fBoundaryToleranceMm;
        CudaError(cudaMemcpyAsync(
                      DevicePointer(fLaunchParamsAllocation->Pointer()),
                      &launchParams, sizeof(launchParams),
                      cudaMemcpyHostToDevice, stream),
                  "cudaMemcpyAsync launch params");

        OptiXError(optixLaunch(pipeline, stream,
                               fLaunchParamsAllocation->Pointer(),
                               sizeof(launchParams), &sbt,
                               static_cast<unsigned int>(photons.size()), 1,
                               1),
                   "optixLaunch");
        CudaError(cudaStreamSynchronize(stream), "cudaStreamSynchronize");

        CudaError(cudaMemcpy(&fDeviceStats,
                             DevicePointer(fStatsAllocation->Pointer()),
                             sizeof(fDeviceStats), cudaMemcpyDeviceToHost),
                  "cudaMemcpy transport stats");
        const auto detectedCount{std::min<std::size_t>(
            fDeviceStats.fDetectedCount, photons.size())};
        CudaError(cudaMemcpy(fDeviceHits.data(),
                             DevicePointer(fHitAllocation->Pointer()),
                             photons.size() * sizeof(DevicePhotonHit),
                             cudaMemcpyDeviceToHost),
                  "cudaMemcpy hit data");
        CudaError(cudaMemcpy(fDeviceHitFlags.data(),
                             DevicePointer(fHitFlagAllocation->Pointer()),
                             photons.size() * sizeof(std::uint32_t),
                             cudaMemcpyDeviceToHost),
                  "cudaMemcpy hit flags");

        result.fStatistics.fDetectedCount = fDeviceStats.fDetectedCount;
        result.fStatistics.fAbsorbedCount = fDeviceStats.fAbsorbedCount;
        result.fStatistics.fEscapedCount = fDeviceStats.fEscapedCount;
        result.fStatistics.fTruncatedCount = fDeviceStats.fTruncatedCount;
        result.fStatistics.fMaxBounceCount = fDeviceStats.fMaxBounceCount;
        result.fStatistics.fInvalidStateCount =
            fDeviceStats.fInvalidStateCount;
        result.fStatistics.fZeroStepCount = fDeviceStats.fZeroStepCount;
        result.fStatistics.fTransportTimeMs =
            std::chrono::duration<double, std::milli>{
                std::chrono::steady_clock::now() - start}
                .count();
        result.fDetections.reserve(detectedCount);
        for (auto index{std::size_t{}}; index < photons.size(); ++index) {
            if (fDeviceHitFlags[index] == 0) {
                continue;
            }
            const auto& hit{fDeviceHits[index]};
            result.fDetections.push_back({
                {hit.fPositionMm[0], hit.fPositionMm[1], hit.fPositionMm[2]},
                hit.fTimeNs,
                {hit.fDirection[0], hit.fDirection[1], hit.fDirection[2]},
                hit.fEnergyEv,
                hit.fEventID,
                hit.fPhotonID,
                hit.fSensorID,
                hit.fFlags,
            });
        }
        return result;
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
        pipelineOptions.numPayloadValues = 5;
        pipelineOptions.numAttributeValues = 2;
        pipelineOptions.pipelineLaunchParamsVariableName = "gLaunchParams";
        pipelineOptions.pipelineLaunchParamsSizeInBytes =
            sizeof(OptixLaunchParams);
        pipelineOptions.usesPrimitiveTypeFlags =
            OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;

        std::array<char, 8192> log{};
        auto logSize{log.size()};
        const auto moduleResult{optixModuleCreate(
            context, &moduleOptions, &pipelineOptions,
            reinterpret_cast<const char*>(g4go_optix_ir),
            static_cast<std::size_t>(g4go_optix_irLength), log.data(), &logSize,
            &module)};
        if (moduleResult != OPTIX_SUCCESS) {
            throw std::runtime_error(
                std::string("optixModuleCreate failed: ") +
                optixGetErrorName(moduleResult) + " (" +
                optixGetErrorString(moduleResult) + ") " +
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
        const auto pipelineResult{optixPipelineCreate(
            context, &pipelineOptions, &linkOptions, programGroups.data(),
            static_cast<unsigned int>(programGroups.size()), log.data(),
            &logSize, &pipeline)};
        if (pipelineResult != OPTIX_SUCCESS) {
            throw std::runtime_error(
                std::string("optixPipelineCreate failed: ") +
                optixGetErrorName(pipelineResult) + " (" +
                optixGetErrorString(pipelineResult) + ") " +
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
        const auto result{optixProgramGroupCreate(
            context, &description, 1, &options, log.data(), &logSize,
            &programGroup)};
        if (result != OPTIX_SUCCESS) {
            throw std::runtime_error(
                std::string(operation) + " failed: " +
                optixGetErrorName(result) + " (" + optixGetErrorString(result) +
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
        sceneAllocations.clear();

        std::vector<DeviceMaterial> materials{};
        materials.reserve(scene.Materials().size());
        for (const auto& material : scene.Materials()) {
            materials.push_back({
                UploadProperty(material.fRindex, sceneAllocations),
                UploadProperty(material.fGroupVelocityMmPerNs,
                               sceneAllocations),
                UploadProperty(material.fAbsLengthMm, sceneAllocations),
            });
        }

        std::vector<DeviceSurface> surfaces{};
        surfaces.reserve(scene.Surfaces().size());
        for (const auto& surface : scene.Surfaces()) {
            surfaces.push_back({
                static_cast<std::uint8_t>(surface.fType),
                static_cast<std::uint8_t>(surface.fModel),
                static_cast<std::uint8_t>(surface.fFinish),
                0,
                surface.fModelValue,
                UploadProperty(surface.fReflectivity, sceneAllocations),
                UploadProperty(surface.fEfficiency, sceneAllocations),
                UploadProperty(surface.fTransmittance, sceneAllocations),
                UploadProperty(surface.fRindex, sceneAllocations),
                UploadProperty(surface.fRealRindex, sceneAllocations),
                UploadProperty(surface.fImaginaryRindex, sceneAllocations),
                UploadProperty(surface.fCoatedRindex, sceneAllocations),
                UploadProperty(surface.fSpecularLobe, sceneAllocations),
                UploadProperty(surface.fSpecularSpike, sceneAllocations),
                UploadProperty(surface.fBackscatter, sceneAllocations),
                UploadProperty(surface.fSurfaceRoughness, sceneAllocations),
                UploadProperty(surface.fDichroic, sceneAllocations),
                surface.fCoatedThicknessMm,
                static_cast<std::uint8_t>(
                    surface.fCoatedFrustratedTransmission),
                {},
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
            std::vector<DeviceVector3> vertices{};
            vertices.reserve(mesh.fVerticesMm.size());
            for (const auto& vertex : mesh.fVerticesMm) {
                vertices.push_back(ToDevice(vertex));
            }
            const auto vertexPointer{
                Upload<DeviceVector3>(vertices, sceneAllocations)};
            const auto indexPointer{
                Upload<std::uint32_t>(mesh.fIndices, sceneAllocations)};
            const auto flagPointer{
                Upload<std::uint8_t>(mesh.fTriangleFlags, sceneAllocations)};
            geometries.push_back({
                {
                 DevicePointerAs<const DeviceVector3>(vertexPointer),
                 DevicePointerAs<const std::uint32_t>(indexPointer),
                 DevicePointerAs<const std::uint8_t>(flagPointer),
                 static_cast<std::uint32_t>(mesh.fVerticesMm.size()),
                 static_cast<std::uint32_t>(mesh.fIndices.size() / 3),
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
                           context, stream, &buildOptions, &buildInput, 1,
                           temporary->Pointer(), bufferSizes.tempSizeInBytes,
                           output->Pointer(), bufferSizes.outputSizeInBytes,
                           &geometryHandle, nullptr, 0),
                       "optixAccelBuild geometry");
            CudaError(cudaStreamSynchronize(stream),
                      "cudaStreamSynchronize geometry GAS");
            sceneAllocations.push_back(std::move(temporary));
            sceneAllocations.push_back(std::move(output));
            geometryHandles.push_back(geometryHandle);
        }

        std::vector<DeviceVolume> volumes{};
        volumes.reserve(scene.Volumes().size());
        std::vector<OptixInstance> instances{};
        instances.reserve(scene.Volumes().size());
        for (auto index{std::size_t{}}; index < scene.Volumes().size(); ++index) {
            const auto& volume{scene.Volumes()[index]};
            if (volume.fGeometryID >= geometryHandles.size()) {
                throw std::invalid_argument("volume references invalid geometry");
            }
            volumes.push_back({
                volume.fVolumeID,
                volume.fPhysicalVolumeID,
                volume.fCopyNo,
                volume.fGeometryID,
                volume.fMaterialID,
                volume.fParentVolumeID,
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
            instance.traversableHandle = geometryHandles[volume.fGeometryID];
            instances.push_back(instance);
        }

        std::vector<DeviceSurfaceBinding> deviceBindings{};
        deviceBindings.reserve(scene.SurfaceBindings().size());
        for (const auto& binding : scene.SurfaceBindings()) {
            deviceBindings.push_back({
                binding.fFromVolumeID,
                binding.fToVolumeID,
                binding.fSurfaceID,
            });
        }

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
            scene.WorldVolumeID(),
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
                       context, stream, &instanceOptions, &instanceInput, 1,
                       instanceTemporary->Pointer(),
                       instanceBufferSizes.tempSizeInBytes,
                       instanceOutput->Pointer(),
                       instanceBufferSizes.outputSizeInBytes,
                       &traversableHandle, nullptr, 0),
                   "optixAccelBuild IAS");
        CudaError(cudaStreamSynchronize(stream),
                  "cudaStreamSynchronize scene IAS");
        sceneAllocations.push_back(std::move(instanceTemporary));
        sceneAllocations.push_back(std::move(instanceOutput));
        sceneAddress = &scene;
    }

    PhotonTransportConfig fConfiguration{};
    OptixDeviceContext context{};
    OptixModule module{};
    OptixProgramGroup raygenProgram{};
    OptixProgramGroup missProgram{};
    OptixProgramGroup hitgroupProgram{};
    OptixPipeline pipeline{};
    cudaStream_t stream{};
    OptixShaderBindingTable sbt{};
    DeviceAllocations sbtAllocations{};
    DeviceAllocations sceneAllocations{};
    DeviceScene deviceScene{};
    const Scene* sceneAddress{};
    OptixTraversableHandle traversableHandle{};
    std::unique_ptr<DeviceAllocation> fPhotonAllocation{};
    std::unique_ptr<DeviceAllocation> fHitAllocation{};
    std::unique_ptr<DeviceAllocation> fHitFlagAllocation{};
    std::unique_ptr<DeviceAllocation> fStatsAllocation{};
    std::unique_ptr<DeviceAllocation> fLaunchParamsAllocation{};
    std::size_t fPhotonCapacity{};
    std::size_t fHitCapacity{};
    std::size_t fHitFlagCapacity{};
    std::size_t fStatsCapacity{};
    std::size_t fLaunchParamsCapacity{};
    std::vector<DevicePhoton> fDevicePhotons{};
    std::vector<DevicePhotonHit> fDeviceHits{};
    std::vector<std::uint32_t> fDeviceHitFlags{};
    DeviceTransportStats fDeviceStats{};
};

OptiXTransportHost::OptiXTransportHost(
    PhotonTransportConfig configuration) :
    fImpl{std::make_unique<Impl>(configuration)} {}

OptiXTransportHost::~OptiXTransportHost() = default;

auto OptiXTransportHost::Propagate(const Scene& scene,
                                   std::span<const Photon> photons)
    -> PhotonTransportOutput {
    return fImpl->Propagate(scene, photons);
}

} // namespace G4GO::Optical
