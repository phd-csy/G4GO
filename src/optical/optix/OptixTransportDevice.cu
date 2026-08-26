#include "BoundaryPhysics.cuh"
#include "OptixTransportTypes.cuh"
#include "math_constants.h"
#include "optix_device.h"

#include <cmath>
#include <cstdint>

namespace G4GO::Optical {

extern "C" {
extern __constant__ OptixLaunchParams gLaunchParams;
}

namespace {

constexpr auto invalidID{0xffffffffU};
constexpr auto dielectricDielectricType{0U};
constexpr auto glisurModel{0U};
constexpr auto unifiedModel{1U};
constexpr auto groundFinish{3U};
constexpr auto groundFrontPaintedFinish{4U};
constexpr auto groundBackPaintedFinish{5U};
constexpr auto speedOfLightMmPerNs{299.792458F};

__device__ __forceinline__ auto Add(DeviceVector3 left, DeviceVector3 right)
    -> DeviceVector3 {
    return {left.fX + right.fX, left.fY + right.fY, left.fZ + right.fZ};
}

__device__ __forceinline__ auto Scale(DeviceVector3 vector, float value)
    -> DeviceVector3 {
    return {vector.fX * value, vector.fY * value, vector.fZ * value};
}

__device__ __forceinline__ auto Dot(DeviceVector3 left, DeviceVector3 right)
    -> float {
    return left.fX * right.fX + left.fY * right.fY + left.fZ * right.fZ;
}

__device__ __forceinline__ auto Normalize(DeviceVector3 vector)
    -> DeviceVector3 {
    const auto lengthSquared{Dot(vector, vector)};
    if (!(lengthSquared > 0.0F) || !isfinite(lengthSquared)) {
        return {};
    }
    return Scale(vector, rsqrtf(lengthSquared));
}

__device__ auto FindVolume(std::uint32_t volumeID) -> const DeviceVolume* {
    for (auto index{0U}; index < gLaunchParams.fScene.fVolumeCount; ++index) {
        if (gLaunchParams.fScene.fVolumes[index].fVolumeID == volumeID) {
            return &gLaunchParams.fScene.fVolumes[index];
        }
    }
    return nullptr;
}

__device__ auto FindMaterial(std::uint32_t materialID)
    -> const DeviceMaterial* {
    return materialID < gLaunchParams.fScene.fMaterialCount
               ? &gLaunchParams.fScene.fMaterials[materialID]
               : nullptr;
}

__device__ auto FindGeometry(std::uint32_t geometryID)
    -> const DeviceGeometry* {
    return geometryID < gLaunchParams.fScene.fGeometryCount
               ? &gLaunchParams.fScene.fGeometries[geometryID]
               : nullptr;
}

__device__ auto TriangleNormal(std::uint32_t instanceIndex,
                               std::uint32_t triangleIndex) -> DeviceVector3 {
    if (instanceIndex >= gLaunchParams.fScene.fVolumeCount) {
        return {};
    }
    const auto& volume{gLaunchParams.fScene.fVolumes[instanceIndex]};
    const auto* geometry{FindGeometry(volume.fGeometryID)};
    if (geometry == nullptr ||
        triangleIndex >= geometry->fMesh.fTriangleCount) {
        return {};
    }
    const auto* indices{geometry->fMesh.fIndices + 3U * triangleIndex};
    const auto& first{geometry->fMesh.fVertices[indices[0]]};
    const auto& second{geometry->fMesh.fVertices[indices[1]]};
    const auto& third{geometry->fMesh.fVertices[indices[2]]};
    const auto normal{Normalize({
        (second.fY - first.fY) * (third.fZ - first.fZ) -
            (second.fZ - first.fZ) * (third.fY - first.fY),
        (second.fZ - first.fZ) * (third.fX - first.fX) -
            (second.fX - first.fX) * (third.fZ - first.fZ),
        (second.fX - first.fX) * (third.fY - first.fY) -
            (second.fY - first.fY) * (third.fX - first.fX),
    })};
    const auto worldNormal{optixTransformNormalFromObjectToWorldSpace(
        make_float3(normal.fX, normal.fY, normal.fZ))};
    return Normalize({worldNormal.x, worldNormal.y, worldNormal.z});
}

__device__ auto SurfaceRank(std::uint32_t currentVolumeID,
                            std::uint32_t candidateVolumeID) -> std::uint32_t {
    const auto* current{FindVolume(currentVolumeID)};
    const auto* candidate{FindVolume(candidateVolumeID)};
    if (current == nullptr || candidate == nullptr) {
        return 100U;
    }
    if (candidateVolumeID == currentVolumeID) {
        return 2U;
    }
    if (candidate->fParentVolumeID == currentVolumeID) {
        return 0U;
    }
    if (candidate->fParentVolumeID == current->fParentVolumeID) {
        return 1U;
    }
    if (candidateVolumeID == current->fParentVolumeID) {
        return 3U;
    }
    return 100U;
}

__device__ auto ResolveTopology(std::uint32_t currentVolumeID,
                                std::uint32_t hitVolumeID)
    -> std::uint32_t {
    const auto* current{FindVolume(currentVolumeID)};
    const auto* hit{FindVolume(hitVolumeID)};
    if (current == nullptr || hit == nullptr) {
        return invalidID;
    }
    if (hitVolumeID == currentVolumeID) {
        return current->fParentVolumeID;
    }
    if (hit->fParentVolumeID == currentVolumeID ||
        hit->fParentVolumeID == current->fParentVolumeID) {
        return hitVolumeID;
    }
    if (hitVolumeID == current->fParentVolumeID) {
        return hitVolumeID;
    }
    return invalidID;
}

__device__ auto SampleProperty(const DeviceProperty& property,
                               float energy,
                               float fallback) -> float {
    if (property.fCount == 0 || property.fEnergyEv == nullptr ||
        property.fValues == nullptr) {
        return fallback;
    }
    if (property.fCount == 1 || energy <= property.fEnergyEv[0]) {
        return property.fValues[0];
    }
    const auto last{property.fCount - 1U};
    if (energy >= property.fEnergyEv[last]) {
        return property.fValues[last];
    }
    auto index{0U};
    while (index + 1U < property.fCount &&
           energy > property.fEnergyEv[index + 1U]) {
        ++index;
    }
    const auto fraction{
        (energy - property.fEnergyEv[index]) /
        (property.fEnergyEv[index + 1U] - property.fEnergyEv[index])};
    return property.fValues[index] +
           fraction * (property.fValues[index + 1U] - property.fValues[index]);
}

__device__ auto FindSurfaceID(std::uint32_t fromVolumeID,
                              std::uint32_t toVolumeID) -> std::uint32_t {
    for (auto index{0U};
         index < gLaunchParams.fScene.fSurfaceBindingCount; ++index) {
        const auto& binding{gLaunchParams.fScene.fSurfaceBindings[index]};
        if (binding.fFromVolumeID == fromVolumeID &&
            binding.fToVolumeID == toVolumeID) {
            return binding.fSurfaceID;
        }
    }
    const auto* toVolume{FindVolume(toVolumeID)};
    if (toVolume != nullptr && toVolume->fSkinSurfaceID != invalidID) {
        return toVolume->fSkinSurfaceID;
    }
    const auto* fromVolume{FindVolume(fromVolumeID)};
    return fromVolume == nullptr ? invalidID : fromVolume->fSkinSurfaceID;
}

__device__ auto Uniform(std::uint64_t seed,
                        std::uint64_t photonID,
                        std::uint32_t bounce,
                        std::uint32_t process,
                        std::uint32_t draw) -> float {
    constexpr auto multiplier0{0xD2511F53U};
    constexpr auto multiplier1{0xCD9E8D57U};
    constexpr auto keyStep0{0x9E3779B9U};
    constexpr auto keyStep1{0xBB67AE85U};
    auto x0{static_cast<std::uint32_t>(photonID)};
    auto x1{static_cast<std::uint32_t>(photonID >> 32)};
    auto x2{bounce};
    auto x3{(process << 16U) ^ draw};
    auto key0{static_cast<std::uint32_t>(seed)};
    auto key1{static_cast<std::uint32_t>(seed >> 32)};
    for (auto round{0}; round < 10; ++round) {
        const auto product0{
            static_cast<unsigned long long>(multiplier0) * x0};
        const auto product1{
            static_cast<unsigned long long>(multiplier1) * x2};
        const auto low0{static_cast<std::uint32_t>(product0)};
        const auto high0{static_cast<std::uint32_t>(product0 >> 32)};
        const auto low1{static_cast<std::uint32_t>(product1)};
        const auto high1{static_cast<std::uint32_t>(product1 >> 32)};
        const auto next0{high1 ^ x1 ^ key0};
        const auto next1{low1};
        const auto next2{high0 ^ x3 ^ key1};
        const auto next3{low0};
        x0 = next0;
        x1 = next1;
        x2 = next2;
        x3 = next3;
        key0 += keyStep0;
        key1 += keyStep1;
    }
    const auto word{draw % 4U == 0U ? x0 : draw % 4U == 1U ? x1 :
                                       draw % 4U == 2U     ? x2 : x3};
    return (static_cast<float>(word) + 0.5F) / 4294967296.0F;
}

__device__ auto SampleAbsorption(const DeviceMaterial& material,
                                 float energyEv,
                                 std::uint64_t seed,
                                 std::uint64_t photonID,
                                 std::uint32_t bounce) -> float {
    const auto absorptionLength{
        SampleProperty(material.fAbsLengthMm, energyEv, CUDART_INF_F)};
    if (!isfinite(absorptionLength) || absorptionLength <= 0.0F) {
        return CUDART_INF_F;
    }
    return -absorptionLength *
           logf(Uniform(seed, photonID, bounce, 0U, 0U));
}

__device__ auto Gaussian(std::uint64_t seed,
                         std::uint64_t photonID,
                         std::uint32_t bounce,
                         std::uint32_t draw) -> float {
    constexpr auto twoPi{6.28318530717958647692F};
    const auto first{fmaxf(Uniform(seed, photonID, bounce, 3U, draw),
                           1.0e-7F)};
    const auto second{Uniform(seed, photonID, bounce, 3U, draw + 1U)};
    return sqrtf(-2.0F * logf(first)) * cosf(twoPi * second);
}

__device__ auto SampleFacetNormal(const DeviceSurface& surface,
                                  DeviceVector3 normal,
                                  std::uint64_t seed,
                                  std::uint64_t photonID,
                                  std::uint32_t bounce) -> DeviceVector3 {
    auto sigma{0.0F};
    if (surface.fModel == unifiedModel) {
        sigma = fmaxf(surface.fModelValue, 0.0F);
    } else if (surface.fModel == glisurModel) {
        const auto polish{fminf(fmaxf(surface.fModelValue, 0.0F), 1.0F)};
        sigma = (1.0F - polish) * 0.5F;
    }
    if (!(sigma > 0.0F)) {
        return Normalize(normal);
    }
    normal = Normalize(normal);
    const auto helper{fabsf(normal.fZ) < 0.9F
                          ? DeviceVector3{0.0F, 0.0F, 1.0F}
                          : DeviceVector3{1.0F, 0.0F, 0.0F}};
    const auto tangent{Normalize({normal.fY * helper.fZ - normal.fZ * helper.fY,
                                 normal.fZ * helper.fX - normal.fX * helper.fZ,
                                 normal.fX * helper.fY - normal.fY * helper.fX})};
    const auto bitangent{Normalize({normal.fY * tangent.fZ -
                                        normal.fZ * tangent.fY,
                                    normal.fZ * tangent.fX -
                                        normal.fX * tangent.fZ,
                                    normal.fX * tangent.fY -
                                        normal.fY * tangent.fX})};
    return Normalize(Add(
        Add(normal, Scale(tangent, sigma * Gaussian(seed, photonID, bounce, 0U))),
        Scale(bitangent, sigma * Gaussian(seed, photonID, bounce, 2U))));
}

__device__ auto SampleSurfaceReflection(const DeviceSurface& surface,
                                        DeviceVector3 direction,
                                        DeviceVector3 normal,
                                        float energyEv,
                                        std::uint64_t seed,
                                        std::uint64_t photonID,
                                        std::uint32_t bounce) -> DeviceVector3 {
    const auto isGround{surface.fFinish == groundFinish ||
                        surface.fFinish == groundFrontPaintedFinish ||
                        surface.fFinish == groundBackPaintedFinish};
    const auto facetNormal{SampleFacetNormal(surface, normal, seed, photonID,
                                             bounce)};
    if (surface.fModel == glisurModel) {
        const auto polish{fminf(fmaxf(surface.fModelValue, 0.0F), 1.0F)};
        if (isGround ||
            Uniform(seed, photonID, bounce, 4U, 0U) >= polish) {
            return BoundaryPhysics::SampleLambertian(
                normal, Uniform(seed, photonID, bounce, 4U, 1U),
                Uniform(seed, photonID, bounce, 4U, 2U));
        }
        return BoundaryPhysics::ReflectDirection(direction, facetNormal);
    }

    const auto spike{fminf(fmaxf(SampleProperty(
                                 surface.fSpecularSpike, energyEv,
                                 isGround ? 0.0F : 1.0F),
                             0.0F),
                          1.0F)};
    const auto lobe{fminf(fmaxf(SampleProperty(surface.fSpecularLobe, energyEv,
                                               0.0F),
                               0.0F),
                         1.0F)};
    const auto backscatter{fminf(fmaxf(SampleProperty(
                                      surface.fBackscatter, energyEv, 0.0F),
                                  0.0F),
                              1.0F)};
    const auto total{spike + lobe + backscatter};
    const auto scale{total > 1.0F ? 1.0F / total : 1.0F};
    const auto sample{Uniform(seed, photonID, bounce, 4U, 0U)};
    const auto backLimit{backscatter * scale};
    const auto lobeLimit{backLimit + lobe * scale};
    if (sample < backLimit) {
        return Scale(direction, -1.0F);
    }
    if (sample < lobeLimit) {
        return BoundaryPhysics::ReflectDirection(direction, facetNormal);
    }
    if (sample < lobeLimit + spike * scale) {
        return BoundaryPhysics::ReflectDirection(direction, normal);
    }
    return BoundaryPhysics::SampleLambertian(
        normal, Uniform(seed, photonID, bounce, 4U, 1U),
        Uniform(seed, photonID, bounce, 4U, 2U));
}

} // namespace

extern "C" {

__constant__ OptixLaunchParams gLaunchParams;

__global__ void __miss__ms() {}

__global__ void __anyhit__ah() {
    const auto photonID{optixGetLaunchIndex().x};
    const auto instanceIndex{optixGetInstanceId()};
    if (instanceIndex >= gLaunchParams.fScene.fVolumeCount) {
        optixIgnoreIntersection();
        return;
    }
    auto selectedVolumeID{optixGetPayload_0()};
    auto normalX{optixGetPayload_1()};
    auto normalY{optixGetPayload_2()};
    auto normalZ{optixGetPayload_3()};
    auto candidateCount{optixGetPayload_4()};
    const auto candidateID{
        gLaunchParams.fScene.fVolumes[instanceIndex].fVolumeID};
    const auto currentID{gLaunchParams.fPhotons[photonID].fVolumeID};
    const auto candidateRank{SurfaceRank(currentID, candidateID)};
    if (candidateRank >= 100U) {
        optixIgnoreIntersection();
        return;
    }
    const auto selectedRank{selectedVolumeID == invalidID
                                ? 100U
                                : SurfaceRank(currentID, selectedVolumeID)};
    if (candidateRank < selectedRank ||
        (candidateRank == selectedRank && candidateID < selectedVolumeID)) {
        selectedVolumeID = candidateID;
        const auto normal{TriangleNormal(instanceIndex,
                                         optixGetPrimitiveIndex())};
        normalX = __float_as_uint(normal.fX);
        normalY = __float_as_uint(normal.fY);
        normalZ = __float_as_uint(normal.fZ);
    }
    ++candidateCount;
    optixSetPayload_0(selectedVolumeID);
    optixSetPayload_1(normalX);
    optixSetPayload_2(normalY);
    optixSetPayload_3(normalZ);
    optixSetPayload_4(candidateCount);
    optixIgnoreIntersection();
}

__global__ void __closesthit__ch() {
    const auto instanceIndex{optixGetInstanceId()};
    if (instanceIndex >= gLaunchParams.fScene.fVolumeCount) {
        return;
    }
    const auto& volume{gLaunchParams.fScene.fVolumes[instanceIndex]};
    const auto normal{TriangleNormal(instanceIndex, optixGetPrimitiveIndex())};
    optixSetPayload_0(__float_as_uint(optixGetRayTmax()));
    optixSetPayload_1(__float_as_uint(normal.fX));
    optixSetPayload_2(__float_as_uint(normal.fY));
    optixSetPayload_3(__float_as_uint(normal.fZ));
    optixSetPayload_4(volume.fVolumeID);
}

__global__ void __raygen__rg() {
    const auto photonID{optixGetLaunchIndex().x};
    if (photonID >= gLaunchParams.fPhotonCount) {
        return;
    }

    auto photon{gLaunchParams.fPhotons[photonID]};
    auto position{photon.fPositionMm};
    auto direction{Normalize(photon.fDirection)};
    auto polarization{BoundaryPhysics::ProjectPolarization(
        photon.fPolarization, direction)};
    auto currentVolumeID{photon.fVolumeID};
    auto terminated{false};

    auto bounce{0U};
    for (; bounce < gLaunchParams.fMaxBounceCount && !terminated; ++bounce) {
        gLaunchParams.fPhotons[photonID].fVolumeID = currentVolumeID;
        atomicMax(&gLaunchParams.fStats->fMaxBounceCount,
                  static_cast<unsigned long long>(bounce + 1U));
        const auto* currentVolume{FindVolume(currentVolumeID)};
        if (currentVolume == nullptr) {
            atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                      static_cast<unsigned long long>(1));
            break;
        }
        const auto* currentMaterial{FindMaterial(currentVolume->fMaterialID)};
        if (currentMaterial == nullptr) {
            atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                      static_cast<unsigned long long>(1));
            break;
        }
        const auto refractiveIndex{
            SampleProperty(currentMaterial->fRindex, photon.fEnergyEv, NAN)};
        const auto velocity{SampleProperty(
            currentMaterial->fGroupVelocityMmPerNs, photon.fEnergyEv,
            isfinite(refractiveIndex) && refractiveIndex > 0.0F
                ? speedOfLightMmPerNs / refractiveIndex
                : NAN)};
        if (!(velocity > 0.0F) || !isfinite(velocity)) {
            atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                      static_cast<unsigned long long>(1));
            break;
        }

        auto payload0{__float_as_uint(CUDART_INF_F)};
        auto payload1{0U};
        auto payload2{0U};
        auto payload3{0U};
        auto payload4{invalidID};
        optixTrace(
            static_cast<OptixTraversableHandle>(gLaunchParams.fTraversable),
            make_float3(position.fX, position.fY, position.fZ),
            make_float3(direction.fX, direction.fY, direction.fZ),
            gLaunchParams.fBoundaryEpsilonMm, CUDART_INF_F, 0.0F,
            OptixVisibilityMask(255), OPTIX_RAY_FLAG_DISABLE_ANYHIT, 0, 1, 0,
            payload0, payload1, payload2, payload3, payload4);

        const auto distance{__uint_as_float(payload0)};
        if (payload4 == invalidID || !isfinite(distance)) {
            atomicAdd(&gLaunchParams.fStats->fEscapedCount,
                      static_cast<unsigned long long>(1));
            break;
        }

        auto hitVolumeID{payload4};
        auto normal{Normalize({__uint_as_float(payload1),
                               __uint_as_float(payload2),
                               __uint_as_float(payload3)})};
        if (currentVolume->fMayHaveCoincidentBoundary != 0) {
            gLaunchParams.fPhotons[photonID].fVolumeID = currentVolumeID;
            auto candidatePayload0{invalidID};
            auto candidatePayload1{0U};
            auto candidatePayload2{0U};
            auto candidatePayload3{0U};
            auto candidatePayload4{0U};
            const auto tolerance{gLaunchParams.fBoundaryEpsilonMm};
            optixTrace(
                static_cast<OptixTraversableHandle>(gLaunchParams.fTraversable),
                make_float3(position.fX, position.fY, position.fZ),
                make_float3(direction.fX, direction.fY, direction.fZ),
                fmaxf(0.0F, distance - tolerance), distance + tolerance, 0.0F,
                OptixVisibilityMask(255),
                OPTIX_RAY_FLAG_ENFORCE_ANYHIT |
                    OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
                0, 1, 0, candidatePayload0, candidatePayload1,
                candidatePayload2, candidatePayload3, candidatePayload4);
            if (candidatePayload0 != invalidID) {
                hitVolumeID = candidatePayload0;
                normal = Normalize({__uint_as_float(candidatePayload1),
                                    __uint_as_float(candidatePayload2),
                                    __uint_as_float(candidatePayload3)});
            }
        }

        const auto absorptionDistance{SampleAbsorption(
            *currentMaterial, photon.fEnergyEv, gLaunchParams.fSeed,
            photon.fPhotonID, bounce)};
        if (absorptionDistance < distance) {
            photon.fTimeNs += absorptionDistance / velocity;
            atomicAdd(&gLaunchParams.fStats->fAbsorbedCount,
                      static_cast<unsigned long long>(1));
            break;
        }
        photon.fTimeNs += distance / velocity;
        const auto boundaryPosition{Add(position, Scale(direction, distance))};

        if (Dot(direction, normal) > 0.0F) {
            normal = Scale(normal, -1.0F);
        }
        const auto nextVolumeID{ResolveTopology(currentVolumeID, hitVolumeID)};
        if (nextVolumeID == invalidID) {
            if (currentVolumeID == gLaunchParams.fScene.fWorldVolumeID &&
                hitVolumeID == currentVolumeID) {
                atomicAdd(&gLaunchParams.fStats->fEscapedCount,
                          static_cast<unsigned long long>(1));
            } else {
                atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                          static_cast<unsigned long long>(1));
            }
            break;
        }
        const auto* nextVolume{FindVolume(nextVolumeID)};
        const auto* nextMaterial{
            nextVolume == nullptr ? nullptr : FindMaterial(nextVolume->fMaterialID)};
        if (nextMaterial == nullptr) {
            atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                      static_cast<unsigned long long>(1));
            break;
        }

        const auto surfaceID{FindSurfaceID(currentVolumeID, nextVolumeID)};
        const auto* surface{
            surfaceID < gLaunchParams.fScene.fSurfaceCount
                ? &gLaunchParams.fScene.fSurfaces[surfaceID]
                : nullptr};
        auto reflected{false};
        auto detected{false};
        auto surfaceAbsorbed{false};
        auto directTransmitted{false};
        if (surface != nullptr) {
            const auto reflectivity{fminf(
                fmaxf(SampleProperty(surface->fReflectivity,
                                     photon.fEnergyEv, 0.0F),
                      0.0F),
                1.0F)};
            const auto efficiency{fminf(
                fmaxf(SampleProperty(surface->fEfficiency,
                                     photon.fEnergyEv, 0.0F),
                      0.0F),
                1.0F)};
            const auto transmittance{fminf(
                fmaxf(SampleProperty(surface->fTransmittance,
                                     photon.fEnergyEv, 0.0F),
                      0.0F),
                1.0F)};
            const auto surfaceOutcome{BoundaryPhysics::ClassifySurface(
                reflectivity, transmittance, surface->fReflectivity.fCount != 0,
                surface->fTransmittance.fCount != 0,
                Uniform(gLaunchParams.fSeed, photon.fPhotonID, bounce, 1U,
                        0U))};
            if (surfaceOutcome == BoundaryPhysics::SurfaceOutcome::Absorb) {
                surfaceAbsorbed = true;
                detected = BoundaryPhysics::EvaluateAbsorption(
                               efficiency,
                               nextVolume->fSensorID != invalidID ||
                                   currentVolume->fSensorID != invalidID,
                               Uniform(gLaunchParams.fSeed, photon.fPhotonID,
                                       bounce, 1U, 3U)) ==
                           BoundaryPhysics::SurfaceOutcome::Detect;
            } else if (surfaceOutcome ==
                       BoundaryPhysics::SurfaceOutcome::DirectTransmit) {
                directTransmitted = true;
            } else {
                if (surface->fType == dielectricDielectricType) {
                    const auto indexTo{SampleProperty(
                        nextMaterial->fRindex, photon.fEnergyEv, NAN)};
                    if (!(refractiveIndex > 0.0F) ||
                        !isfinite(refractiveIndex) || !(indexTo > 0.0F) ||
                        !isfinite(indexTo)) {
                        surfaceAbsorbed = true;
                    } else {
                        const auto interactionNormal{
                            SampleFacetNormal(*surface, normal,
                                              gLaunchParams.fSeed,
                                              photon.fPhotonID, bounce)};
                        const auto fresnel{BoundaryPhysics::ComputeFresnel(
                            direction, polarization, interactionNormal,
                            refractiveIndex, indexTo)};
                        reflected =
                            fresnel.fTotalInternalReflection ||
                            Uniform(gLaunchParams.fSeed, photon.fPhotonID,
                                    bounce, 2U, 0U) >
                                fresnel.fTransmittance;
                        if (reflected) {
                            direction = fresnel.fReflectedDirection;
                            polarization = fresnel.fReflectedPolarization;
                        } else {
                            direction = fresnel.fTransmittedDirection;
                            polarization = fresnel.fTransmittedPolarization;
                            currentVolumeID = nextVolumeID;
                        }
                    }
                } else {
                    reflected = true;
                    direction = SampleSurfaceReflection(
                        *surface, direction, normal, photon.fEnergyEv,
                        gLaunchParams.fSeed, photon.fPhotonID, bounce);
                    polarization = BoundaryPhysics::ReflectPolarization(
                        polarization, normal, direction);
                }
            }
            if (directTransmitted) {
                currentVolumeID = nextVolumeID;
            }
        } else {
            const auto indexTo{
                SampleProperty(nextMaterial->fRindex, photon.fEnergyEv, NAN)};
            if (!(refractiveIndex > 0.0F) || !isfinite(refractiveIndex) ||
                !(indexTo > 0.0F) || !isfinite(indexTo)) {
                surfaceAbsorbed = true;
            } else {
                const auto fresnel{BoundaryPhysics::ComputeFresnel(
                    direction, polarization, normal, refractiveIndex, indexTo)};
                reflected = Uniform(gLaunchParams.fSeed, photon.fPhotonID,
                                    bounce, 2U, 0U) > fresnel.fTransmittance;
                if (reflected) {
                    direction = fresnel.fReflectedDirection;
                    polarization = fresnel.fReflectedPolarization;
                } else {
                    direction = fresnel.fTransmittedDirection;
                    polarization = fresnel.fTransmittedPolarization;
                    currentVolumeID = nextVolumeID;
                }
            }
        }

        if (detected) {
            const auto sensorID{nextVolume->fSensorID != invalidID
                                    ? nextVolume->fSensorID
                                    : currentVolume->fSensorID};
            if (sensorID == invalidID) {
                atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                          static_cast<unsigned long long>(1));
                break;
            }
            gLaunchParams.fHits[photonID] = {
                boundaryPosition,
                photon.fTimeNs,
                direction,
                photon.fEnergyEv,
                photon.fEventID,
                photon.fPhotonID,
                sensorID,
                0,
            };
            gLaunchParams.fHitFlags[photonID] = 1;
            atomicAdd(&gLaunchParams.fStats->fDetectedCount,
                      static_cast<unsigned long long>(1));
            terminated = true;
            break;
        }
        if (surfaceAbsorbed) {
            atomicAdd(&gLaunchParams.fStats->fAbsorbedCount,
                      static_cast<unsigned long long>(1));
            break;
        }
        position = Add(boundaryPosition,
                       Scale(normal, reflected
                                        ? gLaunchParams.fBoundaryEpsilonMm
                                        : -gLaunchParams.fBoundaryEpsilonMm));
    }

    if (!terminated && bounce >= gLaunchParams.fMaxBounceCount) {
        atomicAdd(&gLaunchParams.fStats->fTruncatedCount,
                  static_cast<unsigned long long>(1));
    }
}

} // extern "C"

} // namespace G4GO::Optical
