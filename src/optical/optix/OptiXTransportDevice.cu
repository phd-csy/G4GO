#include "OptiXBoundaryPhysics.cuh"
#include "OptiXDeviceData.cuh"
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
constexpr auto dielectricDielectricType{std::uint32_t{}};
constexpr auto glisurModel{std::uint32_t{}};
constexpr auto unifiedModel{1U};
constexpr auto polishedFinish{std::uint32_t{}};
constexpr auto groundFinish{3U};
constexpr auto groundFrontPaintedFinish{4U};
constexpr auto groundBackPaintedFinish{5U};
constexpr auto speedOfLightMmPerNs{299.792458F};
constexpr auto coincidentTriangleFlag{1U};

__device__ __forceinline__ auto Add(DeviceVector3 left, DeviceVector3 right)
    -> DeviceVector3 {
    return {left.at(0) + right.at(0), left.at(1) + right.at(1),
            left.at(2) + right.at(2)};
}

__device__ __forceinline__ auto Scale(DeviceVector3 vector, float value)
    -> DeviceVector3 {
    return {vector.at(0) * value, vector.at(1) * value,
            vector.at(2) * value};
}

__device__ __forceinline__ auto Dot(DeviceVector3 left, DeviceVector3 right)
    -> float {
    return left.at(0) * right.at(0) + left.at(1) * right.at(1) +
           left.at(2) * right.at(2);
}

__device__ __forceinline__ auto EventStatistics(std::uint32_t eventIndex)
    -> DeviceEventTransportStats* {
    if (gLaunchParams.fEventStats == nullptr ||
        eventIndex >= gLaunchParams.fEventCount) {
        return nullptr;
    }
    return &gLaunchParams.fEventStats[eventIndex];
}

__device__ __forceinline__ auto RecordInvalid(std::uint32_t eventIndex)
    -> void {
    atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
              static_cast<unsigned long long>(1));
    if (auto* statistics{EventStatistics(eventIndex)}; statistics != nullptr) {
        atomicAdd(&statistics->fInvalidStateCount,
                  static_cast<unsigned long long>(1));
    }
}

__device__ __forceinline__ auto RecordEscaped(std::uint32_t eventIndex)
    -> void {
    atomicAdd(&gLaunchParams.fStats->fEscapedCount,
              static_cast<unsigned long long>(1));
    if (auto* statistics{EventStatistics(eventIndex)}; statistics != nullptr) {
        atomicAdd(&statistics->fEscapedCount,
                  static_cast<unsigned long long>(1));
    }
}

__device__ __forceinline__ auto RecordAbsorbed(std::uint32_t eventIndex)
    -> void {
    atomicAdd(&gLaunchParams.fStats->fAbsorbedCount,
              static_cast<unsigned long long>(1));
    if (auto* statistics{EventStatistics(eventIndex)}; statistics != nullptr) {
        atomicAdd(&statistics->fAbsorbedCount,
                  static_cast<unsigned long long>(1));
    }
}

__device__ __forceinline__ auto RecordDetected(std::uint32_t eventIndex)
    -> void {
    atomicAdd(&gLaunchParams.fStats->fDetectedCount,
              static_cast<unsigned long long>(1));
    if (auto* statistics{EventStatistics(eventIndex)}; statistics != nullptr) {
        atomicAdd(&statistics->fDetectedCount,
                  static_cast<unsigned long long>(1));
    }
}

__device__ __forceinline__ auto RecordZeroStep(std::uint32_t eventIndex)
    -> void {
    if (auto* statistics{EventStatistics(eventIndex)}; statistics != nullptr) {
        atomicAdd(&statistics->fZeroStepCount,
                  static_cast<unsigned long long>(1));
    }
    atomicAdd(&gLaunchParams.fStats->fZeroStepCount,
              static_cast<unsigned long long>(1));
}

__device__ __forceinline__ auto RecordBounceCount(std::uint32_t eventIndex,
                                                  std::uint32_t bounceCount)
    -> void {
    atomicMax(&gLaunchParams.fStats->fMaxBounceCount,
              static_cast<unsigned long long>(bounceCount));
    if (auto* statistics{EventStatistics(eventIndex)}; statistics != nullptr) {
        atomicMax(&statistics->fMaxBounceCount,
                  static_cast<unsigned long long>(bounceCount));
    }
}

__device__ __forceinline__ auto RecordTruncated(std::uint32_t eventIndex)
    -> void {
    atomicAdd(&gLaunchParams.fStats->fTruncatedCount,
              static_cast<unsigned long long>(1));
    if (auto* statistics{EventStatistics(eventIndex)}; statistics != nullptr) {
        atomicAdd(&statistics->fTruncatedCount,
                  static_cast<unsigned long long>(1));
    }
}

__device__ __forceinline__ auto Normalize(DeviceVector3 vector)
    -> DeviceVector3 {
    const auto lengthSquared{Dot(vector, vector)};
    if (!(lengthSquared > 0.0F) || !isfinite(lengthSquared)) {
        return {};
    }
    return Scale(vector, rsqrtf(lengthSquared));
}

// OptiXTransportHost remaps public Scene volume IDs to dense device indices.
__device__ auto FindVolume(std::uint32_t volumeIndex) -> const DeviceVolume* {
    return volumeIndex < gLaunchParams.fScene.fVolumeCount ?
               &gLaunchParams.fScene.fVolumes[volumeIndex] :
               nullptr;
}

__device__ auto FindMaterial(std::uint32_t materialID)
    -> const DeviceMaterial* {
    return materialID < gLaunchParams.fScene.fMaterialCount ? &gLaunchParams.fScene.fMaterials[materialID] : nullptr;
}

__device__ auto FindGeometry(std::uint32_t geometryID)
    -> const DeviceGeometry* {
    return geometryID < gLaunchParams.fScene.fGeometryCount ? &gLaunchParams.fScene.fGeometries[geometryID] : nullptr;
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
    DeviceVector3 normal{};
    if (geometry->fMesh.fNormals != nullptr) {
        normal = Normalize(geometry->fMesh.fNormals[triangleIndex]);
    } else {
        const auto* indices{geometry->fMesh.fIndices + 3U * triangleIndex};
        const auto& first{geometry->fMesh.fVertices[indices[0]]};
        const auto& second{geometry->fMesh.fVertices[indices[1]]};
        const auto& third{geometry->fMesh.fVertices[indices[2]]};
        normal = Normalize({
            (second.at(1) - first.at(1)) * (third.at(2) - first.at(2)) -
                (second.at(2) - first.at(2)) * (third.at(1) - first.at(1)),
            (second.at(2) - first.at(2)) * (third.at(0) - first.at(0)) -
                (second.at(0) - first.at(0)) * (third.at(2) - first.at(2)),
            (second.at(0) - first.at(0)) * (third.at(1) - first.at(1)) -
                (second.at(1) - first.at(1)) * (third.at(0) - first.at(0)),
        });
    }
    const auto worldNormal{optixTransformNormalFromObjectToWorldSpace(
        make_float3(normal.at(0), normal.at(1), normal.at(2)))};
    return Normalize({worldNormal.x, worldNormal.y, worldNormal.z});
}

__device__ auto TriangleFlags(std::uint32_t instanceIndex,
                              std::uint32_t triangleIndex) -> std::uint8_t {
    if (instanceIndex >= gLaunchParams.fScene.fVolumeCount) {
        return 0;
    }
    const auto& volume{gLaunchParams.fScene.fVolumes[instanceIndex]};
    const auto* geometry{FindGeometry(volume.fGeometryID)};
    if (geometry == nullptr ||
        triangleIndex >= geometry->fMesh.fTriangleCount ||
        geometry->fMesh.fTriangleFlags == nullptr) {
        return 0;
    }
    return geometry->fMesh.fTriangleFlags[triangleIndex];
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
                               float defaultValue) -> float {
    if (property.fCount == 0 || property.fEnergyEv == nullptr ||
        property.fValues == nullptr) {
        return defaultValue;
    }
    if (property.fConstant != 0 || energy <= property.fEnergyEv[0]) {
        return property.fValues[0];
    }
    const auto last{property.fCount - 1U};
    if (energy >= property.fEnergyEv[last]) {
        return property.fValues[last];
    }
    auto index{std::uint32_t{}};
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
    auto first{std::uint32_t{}};
    auto last{gLaunchParams.fScene.fSurfaceBindingCount};
    while (first < last) {
        const auto middle{first + (last - first) / 2U};
        const auto& binding{gLaunchParams.fScene.fSurfaceBindings[middle]};
        if (binding.fFromVolumeID < fromVolumeID ||
            (binding.fFromVolumeID == fromVolumeID &&
             binding.fToVolumeID < toVolumeID)) {
            first = middle + 1U;
        } else {
            last = middle;
        }
    }
    if (first < gLaunchParams.fScene.fSurfaceBindingCount) {
        const auto& binding{gLaunchParams.fScene.fSurfaceBindings[first]};
        if (binding.fFromVolumeID == fromVolumeID &&
            binding.fToVolumeID == toVolumeID) {
            return binding.fSurfaceID;
        }
    }
    const auto* fromVolume{FindVolume(fromVolumeID)};
    const auto* toVolume{FindVolume(toVolumeID)};
    if (fromVolume == nullptr || toVolume == nullptr) {
        return invalidID;
    }
    if (toVolume->fParentVolumeID == fromVolumeID) {
        if (toVolume->fSkinSurfaceID != invalidID) {
            return toVolume->fSkinSurfaceID;
        }
        return fromVolume->fSkinSurfaceID;
    }
    if (fromVolume->fSkinSurfaceID != invalidID) {
        return fromVolume->fSkinSurfaceID;
    }
    return toVolume->fSkinSurfaceID;
}

__device__ auto Uniform(std::uint64_t seed,
                        std::uint64_t photonKey,
                        std::uint32_t bounce,
                        std::uint32_t process,
                        std::uint32_t draw) -> float {
    constexpr auto multiplier0{0xD2511F53U};
    constexpr auto multiplier1{0xCD9E8D57U};
    constexpr auto keyStep0{0x9E3779B9U};
    constexpr auto keyStep1{0xBB67AE85U};
    auto x0{static_cast<std::uint32_t>(photonKey)};
    auto x1{static_cast<std::uint32_t>(photonKey >> 32)};
    auto x2{bounce};
    auto x3{(process << 16U) ^ draw};
    auto key0{static_cast<std::uint32_t>(seed)};
    auto key1{static_cast<std::uint32_t>(seed >> 32)};
    for (auto round{int{}}; round < 10; ++round) {
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
                                       draw % 4U == 2U     ? x2 :
                                                             x3};
    return (static_cast<float>(word) + 0.5F) / 4294967296.0F;
}

__device__ __forceinline__ auto Cross(DeviceVector3 left,
                                      DeviceVector3 right) -> DeviceVector3 {
    return {left.at(1) * right.at(2) - left.at(2) * right.at(1),
            left.at(2) * right.at(0) - left.at(0) * right.at(2),
            left.at(0) * right.at(1) - left.at(1) * right.at(0)};
}

__device__ auto RotateFromZ(DeviceVector3 vector, DeviceVector3 axis)
    -> DeviceVector3 {
    const auto z{Normalize(axis)};
    const auto helper{
        fabsf(z.at(2)) < 0.999F ? DeviceVector3{0.0F, 0.0F, 1.0F}
            : DeviceVector3{0.0F, 1.0F, 0.0F}
    };
    const auto x{Normalize(Cross(helper, z))};
    const auto y{Cross(z, x)};
    return Normalize(Add(Add(Scale(x, vector.at(0)), Scale(y, vector.at(1))),
                         Scale(z, vector.at(2))));
}

__device__ auto FindEmissionIndex(std::uint32_t photonID)
    -> std::uint32_t {
    if (gLaunchParams.fEmissions == nullptr ||
        gLaunchParams.fEmissionOffsets == nullptr ||
        gLaunchParams.fEmissionCount == 0) {
        return invalidID;
    }
    auto first{std::uint32_t{}};
    auto last{gLaunchParams.fEmissionCount};
    while (first < last) {
        const auto middle{first + (last - first) / 2U};
        if (photonID >= gLaunchParams.fEmissionOffsets[middle + 1U]) {
            first = middle + 1U;
        } else {
            last = middle;
        }
    }
    return first < gLaunchParams.fEmissionCount ? first : invalidID;
}

__device__ auto SampleSpectrum(const DeviceProperty& spectrum,
                               float random) -> float {
    if (spectrum.fCount == 0 || spectrum.fEnergyEv == nullptr ||
        spectrum.fValues == nullptr) {
        return NAN;
    }
    const auto u{fminf(fmaxf(random, 0.0F), 1.0F)};
    if (spectrum.fCount == 1 || u <= spectrum.fValues[0]) {
        return spectrum.fEnergyEv[0];
    }
    const auto last{spectrum.fCount - 1U};
    if (u >= spectrum.fValues[last]) {
        return spectrum.fEnergyEv[last];
    }
    auto index{std::uint32_t{}};
    while (index + 1U < spectrum.fCount &&
           u > spectrum.fValues[index + 1U]) {
        ++index;
    }
    const auto denominator{spectrum.fValues[index + 1U] -
                           spectrum.fValues[index]};
    if (!(denominator > 0.0F)) {
        return spectrum.fEnergyEv[index];
    }
    const auto fraction{(u - spectrum.fValues[index]) / denominator};
    return spectrum.fEnergyEv[index] +
           fraction * (spectrum.fEnergyEv[index + 1U] -
                       spectrum.fEnergyEv[index]);
}

__device__ auto GeneratePhoton(const DeviceOpticalEmission& emission,
                               std::uint32_t photonID,
                               std::uint32_t localPhotonID,
                               std::uint32_t eventIndex)
    -> DevicePhoton {
    DevicePhoton photon{};
    photon.fEventID = emission.fEventID;
    photon.fPhotonID = photonID;
    photon.fVolumeID = emission.fVolumeID;
    photon.fEventIndex = eventIndex;
    photon.fWeight = emission.fWeight;
    photon.fPositionMm = emission.fPositionMm;
    photon.fTimeNs = emission.fTimeNs;
    photon.fDirection = Normalize(emission.fDirection);
    photon.fPolarization = emission.fPolarization;
    photon.fEnergyEv = emission.fEnergyEv;

    const auto photonKey{(static_cast<std::uint64_t>(emission.fEventID) << 32U) |
                         photonID};
    auto draw{std::uint32_t{}};
    if (emission.fType == 1U) {
        const auto* material{FindMaterial(emission.fMaterialID)};
        if (material == nullptr) {
            return photon;
        }
        const auto preVelocity{emission.fPreVelocityMmPerNs};
        const auto beta{(preVelocity +
                         0.5F * emission.fDeltaVelocityMmPerNs) /
                        speedOfLightMmPerNs};
        if (!(beta > 0.0F) || !(beta < 1.0F) ||
            material->fRindex.fCount == 0) {
            return photon;
        }
        const auto& rindex{material->fRindex};
        const auto energyMin{rindex.fEnergyEv[0]};
        const auto energyMax{rindex.fEnergyEv[rindex.fCount - 1U]};
        const auto nMax{material->fRindexMax};
        const auto betaInverse{1.0F / beta};
        const auto maxCos{betaInverse / nMax};
        const auto maxSin2{1.0F - maxCos * maxCos};
        const auto deltaEnergy{energyMax - energyMin};
        if (!(nMax > 0.0F) || !isfinite(nMax) ||
            !(maxCos > 0.0F) || !(maxCos < 1.0F) ||
            !(deltaEnergy >= 0.0F) || !isfinite(deltaEnergy) ||
            !(maxSin2 > 0.0F) || !isfinite(maxSin2)) {
            return photon;
        }
        float sampledEnergy{energyMin};
        float cosTheta{};
        float sin2Theta{};
        auto accepted{bool{}};
        for (auto attempt{std::uint32_t{}}; attempt < 1024U; ++attempt) {
            const auto random{Uniform(gLaunchParams.fSeed, photonKey, 0U,
                                      10U, draw++)};
            sampledEnergy = energyMin + random * deltaEnergy;
            const auto n{SampleProperty(rindex, sampledEnergy, NAN)};
            cosTheta = betaInverse / n;
            sin2Theta = 1.0F - cosTheta * cosTheta;
            if (isfinite(n) && cosTheta > 0.0F && cosTheta < 1.0F &&
                sin2Theta > 0.0F &&
                Uniform(gLaunchParams.fSeed, photonKey, 0U, 10U, draw++) *
                        maxSin2 <=
                    sin2Theta) {
                accepted = true;
                break;
            }
        }
        if (!accepted) {
            return photon;
        }
        const auto phi{6.28318530717958647692F *
                       Uniform(gLaunchParams.fSeed, photonKey, 0U, 10U,
                               draw++)};
        const auto sinTheta{sqrtf(fmaxf(sin2Theta, 0.0F))};
        const auto localDirection{
            DeviceVector3{
                          sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta}
        };
        const auto localPolarization{
            DeviceVector3{
                          cosTheta * cosf(phi), cosTheta * sinf(phi), -sinTheta}
        };
        photon.fDirection = RotateFromZ(localDirection, emission.fDirection);
        photon.fPolarization =
            RotateFromZ(localPolarization, emission.fDirection);

        const auto meanPre{emission.fPreMeanPhotonCount};
        const auto meanPost{emission.fPostMeanPhotonCount};
        const auto deltaMean{meanPre - meanPost};
        const auto maxMean{fmaxf(meanPre, meanPost)};
        float positionFraction{};
        do {
            positionFraction =
                Uniform(gLaunchParams.fSeed, photonKey, 0U, 10U, draw++);
        } while (Uniform(gLaunchParams.fSeed, photonKey, 0U, 10U, draw++) *
                     maxMean >
                 meanPre - positionFraction * deltaMean);
        photon.fPositionMm = Add(
            emission.fPositionMm,
            Scale(emission.fStepDeltaMm, positionFraction));
        const auto velocity{emission.fPreVelocityMmPerNs +
                            0.5F * positionFraction *
                                emission.fDeltaVelocityMmPerNs};
        if (velocity > 0.0F && isfinite(velocity)) {
            photon.fTimeNs +=
                positionFraction * emission.fStepLengthMm / velocity;
        }
        photon.fEnergyEv = sampledEnergy;
        static_cast<void>(localPhotonID);
        return photon;
    }

    if (emission.fType == 2U) {
        const auto* material{FindMaterial(emission.fMaterialID)};
        if (material == nullptr || emission.fSpectrumID >= 3U) {
            return photon;
        }
        photon.fEnergyEv = SampleSpectrum(
            material->fScintillationSpectrum.at(emission.fSpectrumID),
            Uniform(gLaunchParams.fSeed, photonKey, 0U, 20U, draw++));
        const auto cosTheta{1.0F -
                            2.0F * Uniform(gLaunchParams.fSeed, photonKey,
                                           0U, 20U, draw++)};
        const auto sinTheta{sqrtf(fmaxf(0.0F, 1.0F - cosTheta * cosTheta))};
        const auto phi{6.28318530717958647692F *
                       Uniform(gLaunchParams.fSeed, photonKey, 0U, 20U,
                               draw++)};
        const auto localDirection{
            DeviceVector3{
                          sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta}
        };
        auto localPolarization{
            DeviceVector3{
                          cosTheta * cosf(phi), cosTheta * sinf(phi), -sinTheta}
        };
        const auto perpendicular{Cross(localDirection, localPolarization)};
        const auto polarizationPhi{
            6.28318530717958647692F *
            Uniform(gLaunchParams.fSeed, photonKey, 0U, 20U, draw++)};
        localPolarization = Normalize(Add(
            Scale(localPolarization, cosf(polarizationPhi)),
            Scale(perpendicular, sinf(polarizationPhi))));
        photon.fDirection = localDirection;
        photon.fPolarization = localPolarization;

        const auto positionFraction{emission.fCharge == 0.0F ?
                                        1.0F :
                                        Uniform(gLaunchParams.fSeed, photonKey,
                                                0U, 20U, draw++)};
        photon.fPositionMm = Add(
            emission.fPositionMm,
            Scale(emission.fStepDeltaMm, positionFraction));
        const auto velocity{emission.fPreVelocityMmPerNs +
                            0.5F * positionFraction *
                                emission.fDeltaVelocityMmPerNs};
        if (velocity > 0.0F && isfinite(velocity)) {
            photon.fTimeNs +=
                positionFraction * emission.fStepLengthMm / velocity;
        }
        const auto decay{emission.fDecayTimeNs};
        if (decay > 0.0F && isfinite(decay)) {
            if (!(emission.fRiseTimeNs > 0.0F) ||
                !isfinite(emission.fRiseTimeNs)) {
                photon.fTimeNs -=
                    decay * logf(fmaxf(
                                Uniform(gLaunchParams.fSeed, photonKey, 0U, 20U,
                                        draw++),
                                1.0e-7F));
            } else {
                float sample{};
                float acceptance{};
                do {
                    sample = -decay * logf(
                                          fmaxf(1.0e-7F,
                                                1.0F - Uniform(gLaunchParams.fSeed,
                                                               photonKey, 0U, 20U, draw++)));
                    acceptance = Uniform(gLaunchParams.fSeed, photonKey, 0U,
                                         20U, draw++);
                } while (acceptance >
                         (1.0F - expf(-sample / emission.fRiseTimeNs)));
                photon.fTimeNs += sample;
            }
        }
        return photon;
    }

    return photon;
}

__device__ auto SampleAbsorption(const DeviceMaterial& material,
                                 float energyEv,
                                 std::uint64_t seed,
                                 std::uint64_t photonKey,
                                 std::uint32_t bounce) -> float {
    const auto absorptionLength{
        SampleProperty(material.fAbsLengthMm, energyEv, CUDART_INF_F)};
    if (!isfinite(absorptionLength) || absorptionLength <= 0.0F) {
        return CUDART_INF_F;
    }
    return -absorptionLength *
           logf(Uniform(seed, photonKey, bounce, 0U, 0U));
}

__device__ auto SampleFacetNormal(const DeviceSurface& surface,
                                  DeviceVector3 normal,
                                  DeviceVector3 incoming,
                                  std::uint64_t seed,
                                  std::uint64_t photonKey,
                                  std::uint32_t bounce) -> DeviceVector3 {
    normal = Normalize(normal);
    if (surface.fModel == glisurModel) {
        const auto polish{fminf(fmaxf(surface.fModelValue, 0.0F), 1.0F)};
        if (polish >= 1.0F) {
            return normal;
        }
        for (auto attempt{std::uint32_t{}}; attempt < 128U; ++attempt) {
            DeviceVector3 smear{};
            float radiusSquared{};
            do {
                smear = {
                    2.0F * Uniform(seed, photonKey, bounce, 3U,
                                   3U * attempt) -
                        1.0F,
                    2.0F * Uniform(seed, photonKey, bounce, 3U,
                                   3U * attempt + 1U) -
                        1.0F,
                    2.0F * Uniform(seed, photonKey, bounce, 3U,
                                   3U * attempt + 2U) -
                        1.0F,
                };
                radiusSquared = Dot(smear, smear);
            } while (radiusSquared > 1.0F);
            const auto facet{Normalize(Add(
                normal, Scale(smear, 1.0F - polish)))};
            if (Dot(incoming, facet) < 0.0F) {
                return facet;
            }
        }
        return normal;
    }

    if (surface.fModel != unifiedModel) {
        return normal;
    }

    const auto sigma{fmaxf(surface.fModelValue, 0.0F)};
    if (!(sigma > 0.0F)) {
        return normal;
    }
    const auto helper{
        fabsf(normal.at(2)) < 0.9F ? DeviceVector3{0.0F, 0.0F, 1.0F}
            : DeviceVector3{1.0F, 0.0F, 0.0F}
    };
    const auto tangent{Normalize({
        normal.at(1) * helper.at(2) - normal.at(2) * helper.at(1),
        normal.at(2) * helper.at(0) - normal.at(0) * helper.at(2),
        normal.at(0) * helper.at(1) - normal.at(1) * helper.at(0),
    })};
    const auto bitangent{Normalize({
        normal.at(1) * tangent.at(2) - normal.at(2) * tangent.at(1),
        normal.at(2) * tangent.at(0) - normal.at(0) * tangent.at(2),
        normal.at(0) * tangent.at(1) - normal.at(1) * tangent.at(0),
    })};
    constexpr auto halfPi{1.57079632679489661923F};
    const auto sigmaSquared{sigma * sigma};
    for (auto attempt{std::uint32_t{}}; attempt < 256U; ++attempt) {
        const auto alpha{
            halfPi * Uniform(seed, photonKey, bounce, 3U, 6U * attempt)};
        const auto gaussian{expf(-0.5F * alpha * alpha / sigmaSquared)};
        const auto acceptance{gaussian * sinf(alpha)};
        if (Uniform(seed, photonKey, bounce, 3U, 6U * attempt + 1U) >
            acceptance) {
            continue;
        }
        const auto phi{6.28318530717958647692F *
                       Uniform(seed, photonKey, bounce, 3U,
                               6U * attempt + 2U)};
        const auto sinAlpha{sinf(alpha)};
        const auto facet{Normalize(Add(
            Scale(normal, cosf(alpha)),
            Add(Scale(tangent, sinAlpha * cosf(phi)),
                Scale(bitangent, sinAlpha * sinf(phi)))))};
        if (Dot(incoming, facet) < 0.0F) {
            return facet;
        }
    }
    return normal;
}

__device__ auto SampleUnifiedReflection(const DeviceSurface& surface,
                                        DeviceVector3 direction,
                                        DeviceVector3 normal,
                                        float energyEv,
                                        std::uint64_t seed,
                                        std::uint64_t photonKey,
                                        std::uint32_t bounce) -> DeviceVector3 {
    const auto isGround{surface.fFinish == groundFinish ||
                        surface.fFinish == groundFrontPaintedFinish ||
                        surface.fFinish == groundBackPaintedFinish};
    const auto facetNormal{SampleFacetNormal(surface, normal, direction, seed,
                                             photonKey, bounce)};

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
    const auto sample{Uniform(seed, photonKey, bounce, 4U, 0U)};
    if (sample < spike) {
        return BoundaryPhysics::ReflectDirection(direction, normal);
    }
    if (sample < spike + lobe) {
        return BoundaryPhysics::ReflectDirection(direction, facetNormal);
    }
    if (sample < spike + lobe + backscatter) {
        return Scale(direction, -1.0F);
    }
    return BoundaryPhysics::SampleLambertian(
        normal, Uniform(seed, photonKey, bounce, 4U, 1U),
        Uniform(seed, photonKey, bounce, 4U, 2U));
}

__device__ auto SampleSurfaceReflection(const DeviceSurface& surface,
                                        DeviceVector3 direction,
                                        DeviceVector3 normal,
                                        float energyEv,
                                        std::uint64_t seed,
                                        std::uint64_t photonKey,
                                        std::uint32_t bounce) -> DeviceVector3 {
    if (surface.fFinish == polishedFinish) {
        return BoundaryPhysics::ReflectDirection(direction, normal);
    }
    if (surface.fModel == glisurModel) {
        const auto facetNormal{SampleFacetNormal(surface, normal, direction,
                                                 seed, photonKey, bounce)};
        return BoundaryPhysics::ReflectDirection(direction, facetNormal);
    }
    return SampleUnifiedReflection(surface, direction, normal, energyEv, seed,
                                   photonKey, bounce);
}

} // namespace

extern "C" {

__constant__ OptixLaunchParams gLaunchParams;

__global__ auto __miss__ms() -> void {}

__global__ auto __anyhit__ah() -> void {
    const auto instanceIndex{optixGetInstanceId()};
    if (instanceIndex >= gLaunchParams.fScene.fVolumeCount) {
        optixIgnoreIntersection();
        return;
    }
    if ((TriangleFlags(instanceIndex, optixGetPrimitiveIndex()) &
         coincidentTriangleFlag) == 0U) {
        optixIgnoreIntersection();
        return;
    }
    auto selectedVolumeID{optixGetPayload_0()};
    auto normalX{optixGetPayload_1()};
    auto normalY{optixGetPayload_2()};
    auto normalZ{optixGetPayload_3()};
    auto candidateCount{optixGetPayload_4()};
    const auto currentID{optixGetPayload_5()};
    const auto candidateID{
        gLaunchParams.fScene.fVolumes[instanceIndex].fVolumeID};
    const auto candidateRank{SurfaceRank(currentID, candidateID)};
    if (candidateRank >= 100U) {
        optixIgnoreIntersection();
        return;
    }
    const auto selectedRank{selectedVolumeID == invalidID ? 100U : SurfaceRank(currentID, selectedVolumeID)};
    if (candidateRank < selectedRank ||
        (candidateRank == selectedRank && candidateID < selectedVolumeID)) {
        selectedVolumeID = candidateID;
        const auto normal{TriangleNormal(instanceIndex,
                                         optixGetPrimitiveIndex())};
        normalX = __float_as_uint(normal.at(0));
        normalY = __float_as_uint(normal.at(1));
        normalZ = __float_as_uint(normal.at(2));
    }
    ++candidateCount;
    optixSetPayload_0(selectedVolumeID);
    optixSetPayload_1(normalX);
    optixSetPayload_2(normalY);
    optixSetPayload_3(normalZ);
    optixSetPayload_4(candidateCount);
    optixSetPayload_5(currentID);
    optixIgnoreIntersection();
}

__global__ auto __closesthit__ch() -> void {
    const auto instanceIndex{optixGetInstanceId()};
    if (instanceIndex >= gLaunchParams.fScene.fVolumeCount) {
        return;
    }
    const auto& volume{gLaunchParams.fScene.fVolumes[instanceIndex]};
    const auto normal{TriangleNormal(instanceIndex, optixGetPrimitiveIndex())};
    optixSetPayload_0(__float_as_uint(optixGetRayTmax()));
    optixSetPayload_1(__float_as_uint(normal.at(0)));
    optixSetPayload_2(__float_as_uint(normal.at(1)));
    optixSetPayload_3(__float_as_uint(normal.at(2)));
    optixSetPayload_4(volume.fVolumeID);
    optixSetPayload_5(TriangleFlags(instanceIndex, optixGetPrimitiveIndex()));
}

__global__ auto __raygen__rg() -> void {
    const auto photonID{optixGetLaunchIndex().x};
    if (photonID >= gLaunchParams.fPhotonCount) {
        return;
    }

    const auto emissionIndex{FindEmissionIndex(photonID)};
    if (emissionIndex == invalidID) {
        atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                  static_cast<unsigned long long>(1));
        return;
    }
    if (gLaunchParams.fEmissionEventIndices == nullptr) {
        atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                  static_cast<unsigned long long>(1));
        return;
    }
    const auto eventIndex{gLaunchParams.fEmissionEventIndices[emissionIndex]};
    if (eventIndex >= gLaunchParams.fEventCount) {
        atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                  static_cast<unsigned long long>(1));
        return;
    }
    const auto& emission{gLaunchParams.fEmissions[emissionIndex]};
    const auto emissionOffset{gLaunchParams.fEmissionOffsets[emissionIndex]};
    const auto localPhotonID{photonID - emissionOffset};
    if (localPhotonID >= emission.fPhotonCount) {
        RecordInvalid(eventIndex);
        return;
    }
    auto photon{GeneratePhoton(emission,
                               emission.fFirstPhotonID + localPhotonID,
                               localPhotonID, eventIndex)};
    const auto photonEventIndex{photon.fEventIndex};
    if (photonEventIndex >= gLaunchParams.fEventCount) {
        RecordInvalid(eventIndex);
        return;
    }
    const auto directionLengthSquared{Dot(photon.fDirection,
                                          photon.fDirection)};
    if (!(photon.fEnergyEv > 0.0F) || !isfinite(photon.fEnergyEv) ||
        !(directionLengthSquared > 0.0F) ||
        !isfinite(directionLengthSquared)) {
        RecordInvalid(photonEventIndex);
        return;
    }
    const auto photonKey{(static_cast<std::uint64_t>(photon.fEventID) << 32U) |
                         photon.fPhotonID};
    auto position{photon.fPositionMm};
    auto direction{Normalize(photon.fDirection)};
    auto polarization{BoundaryPhysics::ProjectPolarization(
        photon.fPolarization, direction)};
    auto currentVolumeID{photon.fVolumeID};
    auto terminated{bool{}};

    auto bounce{std::uint32_t{}};
    auto bounceCount{std::uint32_t{}};
    for (; bounce < gLaunchParams.fMaxBounceCount && !terminated; ++bounce) {
        ++bounceCount;
        const auto* currentVolume{FindVolume(currentVolumeID)};
        if (currentVolume == nullptr) {
            RecordInvalid(photonEventIndex);
            break;
        }
        const auto* currentMaterial{FindMaterial(currentVolume->fMaterialID)};
        if (currentMaterial == nullptr) {
            RecordInvalid(photonEventIndex);
            break;
        }
        const auto refractiveIndex{
            SampleProperty(currentMaterial->fRindex, photon.fEnergyEv, NAN)};
        const auto velocity{SampleProperty(
            currentMaterial->fGroupVelocityMmPerNs, photon.fEnergyEv,
            isfinite(refractiveIndex) && refractiveIndex > 0.0F ? speedOfLightMmPerNs / refractiveIndex : NAN)};
        if (!(velocity > 0.0F) || !isfinite(velocity)) {
            RecordInvalid(photonEventIndex);
            break;
        }

        auto payload0{__float_as_uint(CUDART_INF_F)};
        auto payload1{std::uint32_t{}};
        auto payload2{std::uint32_t{}};
        auto payload3{std::uint32_t{}};
        auto payload4{invalidID};
        auto payload5{std::uint32_t{}};
        optixTrace(
            static_cast<OptixTraversableHandle>(gLaunchParams.fTraversable),
            make_float3(position.at(0), position.at(1), position.at(2)),
            make_float3(direction.at(0), direction.at(1), direction.at(2)),
            gLaunchParams.fBoundaryEpsilonMm, CUDART_INF_F, 0.0F,
            OptixVisibilityMask(255), OPTIX_RAY_FLAG_DISABLE_ANYHIT, 0, 1, 0,
            payload0, payload1, payload2, payload3, payload4, payload5);

        const auto distance{__uint_as_float(payload0)};
        if (payload4 == invalidID || !isfinite(distance)) {
            RecordEscaped(photonEventIndex);
            break;
        }
        if (distance <= gLaunchParams.fBoundaryEpsilonMm) {
            RecordZeroStep(photonEventIndex);
        }

        auto hitVolumeID{payload4};
        auto normal{Normalize({__uint_as_float(payload1),
                               __uint_as_float(payload2),
                               __uint_as_float(payload3)})};
        if (currentVolume->fMayHaveCoincidentBoundary != 0 &&
            (payload5 & coincidentTriangleFlag) != 0U) {
            if (gLaunchParams.fEnablePerformanceDiagnostics != 0) {
                atomicAdd(&gLaunchParams.fStats->fCoincidentCandidateTraceCount,
                          static_cast<unsigned long long>(1));
            }
            auto candidatePayload0{invalidID};
            auto candidatePayload1{std::uint32_t{}};
            auto candidatePayload2{std::uint32_t{}};
            auto candidatePayload3{std::uint32_t{}};
            auto candidatePayload4{std::uint32_t{}};
            auto candidatePayload5{currentVolumeID};
            const auto tolerance{gLaunchParams.fBoundaryEpsilonMm};
            optixTrace(
                static_cast<OptixTraversableHandle>(gLaunchParams.fTraversable),
                make_float3(position.at(0), position.at(1), position.at(2)),
                make_float3(direction.at(0), direction.at(1), direction.at(2)),
                fmaxf(0.0F, distance - tolerance), distance + tolerance, 0.0F,
                OptixVisibilityMask(255),
                OPTIX_RAY_FLAG_ENFORCE_ANYHIT |
                    OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
                0, 1, 0, candidatePayload0, candidatePayload1,
                candidatePayload2, candidatePayload3, candidatePayload4,
                candidatePayload5);
            if (candidatePayload0 != invalidID) {
                hitVolumeID = candidatePayload0;
                normal = Normalize({__uint_as_float(candidatePayload1),
                                    __uint_as_float(candidatePayload2),
                                    __uint_as_float(candidatePayload3)});
                if (gLaunchParams.fEnablePerformanceDiagnostics != 0) {
                    atomicAdd(
                        &gLaunchParams.fStats->fCoincidentCandidateHitCount,
                        static_cast<unsigned long long>(1));
                }
            }
        }

        const auto absorptionDistance{SampleAbsorption(
            *currentMaterial, photon.fEnergyEv, gLaunchParams.fSeed,
            photonKey, bounce)};
        if (absorptionDistance < distance) {
            photon.fTimeNs += absorptionDistance / velocity;
            RecordAbsorbed(photonEventIndex);
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
                RecordEscaped(photonEventIndex);
            } else {
                RecordInvalid(photonEventIndex);
            }
            break;
        }
        const auto* nextVolume{FindVolume(nextVolumeID)};
        const auto* nextMaterial{
            nextVolume == nullptr ? nullptr : FindMaterial(nextVolume->fMaterialID)};
        if (nextMaterial == nullptr) {
            RecordInvalid(photonEventIndex);
            break;
        }

        const auto surfaceID{FindSurfaceID(currentVolumeID, nextVolumeID)};
        const auto* surface{
            surfaceID < gLaunchParams.fScene.fSurfaceCount ? &gLaunchParams.fScene.fSurfaces[surfaceID] : nullptr};
        auto reflected{bool{}};
        auto detected{bool{}};
        auto surfaceAbsorbed{bool{}};
        auto directTransmitted{bool{}};
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
                Uniform(gLaunchParams.fSeed, photonKey, bounce, 1U,
                        0U))};
            if (surfaceOutcome == BoundaryPhysics::SurfaceOutcome::Absorb) {
                surfaceAbsorbed = true;
                detected = BoundaryPhysics::EvaluateAbsorption(
                               efficiency,
                               nextVolume->fSensorID != invalidID ||
                                   currentVolume->fSensorID != invalidID,
                               Uniform(gLaunchParams.fSeed, photonKey,
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
                            surface->fFinish == polishedFinish ?
                                Normalize(normal) :
                                SampleFacetNormal(*surface, normal, direction,
                                                  gLaunchParams.fSeed,
                                                  photonKey, bounce)};
                        const auto roughness{SampleProperty(
                            surface->fSurfaceRoughness, photon.fEnergyEv,
                            0.0F)};
                        auto roughnessPass{true};
                        if (roughness > 0.0F && refractiveIndex > indexTo &&
                            isfinite(roughness) &&
                            isfinite(photon.fEnergyEv) &&
                            photon.fEnergyEv > 0.0F) {
                            constexpr auto pi{3.14159265358979323846F};
                            constexpr auto hcEvMm{1.239841984e-3F};
                            const auto cost1{fmaxf(
                                0.0F, -Dot(direction, interactionNormal))};
                            const auto wavelength{hcEvMm / photon.fEnergyEv};
                            const auto exponent{
                                4.0F * pi * roughness * refractiveIndex *
                                cost1 / wavelength};
                            const auto criterion{expf(-exponent * exponent)};
                            roughnessPass =
                                Uniform(gLaunchParams.fSeed, photonKey,
                                        bounce, 2U, 3U) < criterion;
                        }
                        const auto fresnel{BoundaryPhysics::ComputeFresnel(
                            direction, polarization, interactionNormal,
                            refractiveIndex, indexTo)};
                        reflected =
                            fresnel.fTotalInternalReflection ||
                            Uniform(gLaunchParams.fSeed, photonKey,
                                    bounce, 2U, 0U) >
                                fresnel.fTransmittance;
                        if (reflected) {
                            if (roughnessPass) {
                                if (surface->fModel == unifiedModel &&
                                    surface->fFinish != polishedFinish) {
                                    direction = SampleUnifiedReflection(
                                        *surface, direction, normal,
                                        photon.fEnergyEv,
                                        gLaunchParams.fSeed, photonKey, bounce);
                                    polarization =
                                        BoundaryPhysics::ReflectPolarization(
                                            polarization, normal, direction);
                                } else {
                                    direction = fresnel.fReflectedDirection;
                                    polarization =
                                        fresnel.fReflectedPolarization;
                                }
                            } else {
                                direction = BoundaryPhysics::SampleLambertian(
                                    normal,
                                    Uniform(gLaunchParams.fSeed, photonKey,
                                            bounce, 2U, 4U),
                                    Uniform(gLaunchParams.fSeed, photonKey,
                                            bounce, 2U, 5U));
                                polarization = BoundaryPhysics::ProjectPolarization(
                                    polarization, direction);
                            }
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
                        gLaunchParams.fSeed, photonKey, bounce);
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
                reflected = Uniform(gLaunchParams.fSeed, photonKey,
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
            const auto sensorID{nextVolume->fSensorID != invalidID ? nextVolume->fSensorID : currentVolume->fSensorID};
            if (sensorID == invalidID) {
                RecordInvalid(photonEventIndex);
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
            RecordDetected(photonEventIndex);
            terminated = true;
            break;
        }
        if (surfaceAbsorbed) {
            RecordAbsorbed(photonEventIndex);
            break;
        }
        position = Add(boundaryPosition,
                       Scale(normal, reflected ? gLaunchParams.fBoundaryEpsilonMm : -gLaunchParams.fBoundaryEpsilonMm));
    }

    const auto executedBounceCount{
        bounce >= gLaunchParams.fMaxBounceCount ?
            gLaunchParams.fMaxBounceCount :
            bounceCount};
    RecordBounceCount(photonEventIndex, executedBounceCount);
    if (gLaunchParams.fEnablePerformanceDiagnostics != 0) {
        atomicAdd(&gLaunchParams.fStats->fTotalBounceCount,
                  static_cast<unsigned long long>(bounceCount));
    }
    if (!terminated && bounce >= gLaunchParams.fMaxBounceCount) {
        RecordTruncated(photonEventIndex);
    }
}

} // extern "C"

} // namespace G4GO::Optical
