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
constexpr auto boxKind{0U};
constexpr auto dielectricMetal{1U};
constexpr auto groundFinish{1U};
constexpr auto speedOfLightMmPerNs{299.792458F};
constexpr auto pi{3.14159265358979323846F};
constexpr auto twoPi{2.0F * pi};

struct Intersection {
    float distance{};
    DeviceVector3 normal{};
    bool hit{};
};

__device__ __forceinline__ auto Add(DeviceVector3 left, DeviceVector3 right)
    -> DeviceVector3 {
    return {left.fX + right.fX, left.fY + right.fY, left.fZ + right.fZ};
}

__device__ __forceinline__ auto Subtract(DeviceVector3 left,
                                         DeviceVector3 right)
    -> DeviceVector3 {
    return {left.fX - right.fX, left.fY - right.fY, left.fZ - right.fZ};
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

__device__ __forceinline__ auto PhotonRandomID(const DevicePhoton& photon)
    -> unsigned long long {
    return (static_cast<unsigned long long>(photon.fEventID) << 32U) |
           photon.fPhotonID;
}

__device__ __forceinline__ auto RotateToWorld(const DeviceRotation& rotation,
                                              DeviceVector3 vector)
    -> DeviceVector3 {
    return {
        rotation.fXX * vector.fX + rotation.fXY * vector.fY +
            rotation.fXZ * vector.fZ,
        rotation.fYX * vector.fX + rotation.fYY * vector.fY +
            rotation.fYZ * vector.fZ,
        rotation.fZX * vector.fX + rotation.fZY * vector.fY +
            rotation.fZZ * vector.fZ,
    };
}

__device__ __forceinline__ auto RotateToLocal(const DeviceRotation& rotation,
                                              DeviceVector3 vector)
    -> DeviceVector3 {
    return {
        rotation.fXX * vector.fX + rotation.fYX * vector.fY +
            rotation.fZX * vector.fZ,
        rotation.fXY * vector.fX + rotation.fYY * vector.fY +
            rotation.fZY * vector.fZ,
        rotation.fXZ * vector.fX + rotation.fYZ * vector.fY +
            rotation.fZZ * vector.fZ,
    };
}

__device__ __forceinline__ auto ToLocal(const DeviceSolid& solid,
                                        DeviceVector3 point) -> DeviceVector3 {
    return RotateToLocal(solid.fRotation, Subtract(point, solid.fTranslationMm));
}

__device__ __forceinline__ auto PhiInRange(float phi,
                                           float startPhi,
                                           float deltaPhi) -> bool {
    if (deltaPhi >= twoPi - 1.0e-5F) {
        return true;
    }
    auto relative{fmodf(phi - startPhi, twoPi)};
    if (relative < 0.0F) {
        relative += twoPi;
    }
    return relative <= deltaPhi;
}

__device__ __forceinline__ auto Contains(const DeviceSolid& solid,
                                         DeviceVector3 point) -> bool {
    const auto local{ToLocal(solid, point)};
    if (solid.fKind == boxKind) {
        return fabsf(local.fX) <= solid.fHalfSizeMm.fX + 1.0e-5F &&
               fabsf(local.fY) <= solid.fHalfSizeMm.fY + 1.0e-5F &&
               fabsf(local.fZ) <= solid.fHalfSizeMm.fZ + 1.0e-5F;
    }

    const auto radiusSquared{local.fX * local.fX + local.fY * local.fY};
    return fabsf(local.fZ) <= solid.fHalfLengthMm + 1.0e-5F &&
           radiusSquared >= solid.fInnerRadiusMm * solid.fInnerRadiusMm -
                                1.0e-5F &&
           radiusSquared <= solid.fOuterRadiusMm * solid.fOuterRadiusMm +
                                1.0e-5F &&
           PhiInRange(atan2f(local.fY, local.fX), solid.fStartPhi,
                      solid.fDeltaPhi);
}

__device__ __forceinline__ auto AddCandidate(float distance,
                                             DeviceVector3 normal,
                                             float epsilon,
                                             Intersection& result) -> void {
    if (isfinite(distance) && distance > epsilon &&
        (!result.hit || distance < result.distance)) {
        result.hit = true;
        result.distance = distance;
        result.normal = normal;
    }
}

__device__ auto IntersectBox(const DeviceSolid& solid,
                             DeviceVector3 origin,
                             DeviceVector3 direction,
                             float epsilon) -> Intersection {
    const auto localOrigin{ToLocal(solid, origin)};
    const auto localDirection{RotateToLocal(solid.fRotation, direction)};
    const auto halfSize{solid.fHalfSizeMm};
    const float position[3]{localOrigin.fX, localOrigin.fY, localOrigin.fZ};
    const float component[3]{localDirection.fX, localDirection.fY,
                             localDirection.fZ};
    const float half[3]{halfSize.fX, halfSize.fY, halfSize.fZ};
    auto tMin{-CUDART_INF_F};
    auto tMax{CUDART_INF_F};
    auto nearAxis{0};
    auto farAxis{0};

    for (auto axis{0}; axis < 3; ++axis) {
        if (fabsf(component[axis]) < 1.0e-8F) {
            if (fabsf(position[axis]) > half[axis]) {
                return {};
            }
            continue;
        }
        auto first{(-half[axis] - position[axis]) / component[axis]};
        auto second{(half[axis] - position[axis]) / component[axis]};
        if (first > second) {
            const auto temporary{first};
            first = second;
            second = temporary;
        }
        if (first > tMin) {
            tMin = first;
            nearAxis = axis;
        }
        if (second < tMax) {
            tMax = second;
            farAxis = axis;
        }
        if (tMin > tMax) {
            return {};
        }
    }

    const auto distance{tMin > epsilon ? tMin : tMax};
    if (!isfinite(distance) || distance <= epsilon) {
        return {};
    }
    const auto axis{tMin > epsilon ? nearAxis : farAxis};
    const auto sign{component[axis] > 0.0F ? -1.0F : 1.0F};
    DeviceVector3 localNormal{};
    if (axis == 0) {
        localNormal.fX = sign;
    } else if (axis == 1) {
        localNormal.fY = sign;
    } else {
        localNormal.fZ = sign;
    }
    return {distance, RotateToWorld(solid.fRotation, localNormal), true};
}

__device__ auto IntersectTub(const DeviceSolid& solid,
                             DeviceVector3 origin,
                             DeviceVector3 direction,
                             float epsilon) -> Intersection {
    const auto localOrigin{ToLocal(solid, origin)};
    const auto localDirection{RotateToLocal(solid.fRotation, direction)};
    Intersection result{};

    const auto addCylinder{[&](float radius, bool inner) {
        if (radius <= 0.0F) {
            return;
        }
        const auto a{localDirection.fX * localDirection.fX +
                     localDirection.fY * localDirection.fY};
        const auto b{2.0F * (localOrigin.fX * localDirection.fX +
                             localOrigin.fY * localDirection.fY)};
        const auto c{localOrigin.fX * localOrigin.fX +
                     localOrigin.fY * localOrigin.fY - radius * radius};
        if (fabsf(a) < 1.0e-8F) {
            return;
        }
        const auto discriminant{b * b - 4.0F * a * c};
        if (discriminant < 0.0F) {
            return;
        }
        const auto root{sqrtf(fmaxf(discriminant, 0.0F))};
        const float roots[2]{
            (-b - root) / (2.0F * a),
            (-b + root) / (2.0F * a),
        };
        for (const auto distance : roots) {
            if (distance <= epsilon) {
                continue;
            }
            const auto point{Add(localOrigin, Scale(localDirection, distance))};
            if (point.fZ < -solid.fHalfLengthMm - epsilon ||
                point.fZ > solid.fHalfLengthMm + epsilon ||
                !PhiInRange(atan2f(point.fY, point.fX), solid.fStartPhi,
                            solid.fDeltaPhi)) {
                continue;
            }
            auto normal{Normalize({point.fX, point.fY, 0.0F})};
            if (inner) {
                normal = Scale(normal, -1.0F);
            }
            AddCandidate(distance, normal, epsilon, result);
        }
    }};

    addCylinder(solid.fOuterRadiusMm, false);
    addCylinder(solid.fInnerRadiusMm, true);

    if (fabsf(localDirection.fZ) >= 1.0e-8F) {
        const float capZ[2]{solid.fHalfLengthMm, -solid.fHalfLengthMm};
        const float capNormal[2]{1.0F, -1.0F};
        for (auto index{0}; index < 2; ++index) {
            const auto distance{
                (capZ[index] - localOrigin.fZ) / localDirection.fZ};
            if (distance <= epsilon) {
                continue;
            }
            const auto point{Add(localOrigin, Scale(localDirection, distance))};
            const auto radialSquared{point.fX * point.fX + point.fY * point.fY};
            if (radialSquared >= solid.fInnerRadiusMm * solid.fInnerRadiusMm -
                                     epsilon &&
                radialSquared <= solid.fOuterRadiusMm * solid.fOuterRadiusMm +
                                     epsilon) {
                AddCandidate(distance, {0.0F, 0.0F, capNormal[index]}, epsilon,
                             result);
            }
        }
    }

    if (result.hit) {
        result.normal = RotateToWorld(solid.fRotation, result.normal);
    }
    return result;
}

__device__ auto Intersect(const DeviceSolid& solid,
                          DeviceVector3 origin,
                          DeviceVector3 direction,
                          float epsilon) -> Intersection {
    return solid.fKind == boxKind ? IntersectBox(solid, origin, direction, epsilon) : IntersectTub(solid, origin, direction, epsilon);
}

__device__ auto LocateVolume(DeviceVector3 point) -> std::uint32_t {
    auto volumeID{invalidID};
    auto depth{0U};
    for (auto index{0U}; index < gLaunchParams.fScene.fSolidCount; ++index) {
        const auto& solid{gLaunchParams.fScene.fSolids[index]};
        if (Contains(solid, point) &&
            (volumeID == invalidID || solid.fDepth > depth ||
             (solid.fDepth == depth && solid.fVolumeID < volumeID))) {
            volumeID = solid.fVolumeID;
            depth = solid.fDepth;
        }
    }
    return volumeID;
}

__device__ auto FindSolid(std::uint32_t volumeID) -> const DeviceSolid* {
    for (auto index{0U}; index < gLaunchParams.fScene.fSolidCount; ++index) {
        if (gLaunchParams.fScene.fSolids[index].fVolumeID == volumeID) {
            return &gLaunchParams.fScene.fSolids[index];
        }
    }
    return nullptr;
}

__device__ auto FindMaterial(std::uint32_t materialID)
    -> const DeviceMaterial* {
    return materialID < gLaunchParams.fScene.fMaterialCount ? &gLaunchParams.fScene.fMaterials[materialID] : nullptr;
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
    const auto last{property.fCount - 1};
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
    const auto* toSolid{FindSolid(toVolumeID)};
    if (toSolid != nullptr && toSolid->fSkinSurfaceID != invalidID) {
        return toSolid->fSkinSurfaceID;
    }
    const auto* fromSolid{FindSolid(fromVolumeID)};
    return fromSolid == nullptr ? invalidID : fromSolid->fSkinSurfaceID;
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
                                       draw % 4U == 2U     ? x2 :
                                                             x3};
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
    return -absorptionLength * logf(Uniform(seed, photonID, bounce, 0U, 0U));
}

} // namespace

extern "C" {

__constant__ OptixLaunchParams gLaunchParams;

__global__ void __miss__ms() {
    optixSetPayload_0(__float_as_uint(CUDART_INF_F));
    optixSetPayload_4(invalidID);
}

__global__ void __intersection__is() {
    const auto primitiveIndex{optixGetPrimitiveIndex()};
    if (primitiveIndex >= gLaunchParams.fScene.fSolidCount) {
        return;
    }
    const auto& solid{gLaunchParams.fScene.fSolids[primitiveIndex]};
    const auto origin{optixGetObjectRayOrigin()};
    const auto direction{optixGetObjectRayDirection()};
    const auto intersection{Intersect(
        solid, {origin.x, origin.y, origin.z},
        {direction.x, direction.y, direction.z},
        gLaunchParams.fBoundaryEpsilonMm)};
    if (intersection.hit) {
        optixReportIntersection(intersection.distance, 0);
    }
}

__global__ void __closesthit__ch() {
    const auto primitiveIndex{optixGetPrimitiveIndex()};
    const auto& solid{gLaunchParams.fScene.fSolids[primitiveIndex]};
    const auto origin{optixGetWorldRayOrigin()};
    const auto direction{optixGetWorldRayDirection()};
    const auto intersection{Intersect(
        solid, {origin.x, origin.y, origin.z},
        {direction.x, direction.y, direction.z},
        gLaunchParams.fBoundaryEpsilonMm)};
    optixSetPayload_0(__float_as_uint(intersection.distance));
    optixSetPayload_1(__float_as_uint(intersection.normal.fX));
    optixSetPayload_2(__float_as_uint(intersection.normal.fY));
    optixSetPayload_3(__float_as_uint(intersection.normal.fZ));
    optixSetPayload_4(solid.fVolumeID);
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
        atomicMax(&gLaunchParams.fStats->fMaxBounceCount,
                  static_cast<unsigned long long>(bounce + 1U));
        const auto* currentSolid{FindSolid(currentVolumeID)};
        if (currentSolid == nullptr) {
            atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                      static_cast<unsigned long long>(1));
            break;
        }
        const auto* currentMaterial{FindMaterial(currentSolid->fMaterialID)};
        if (currentMaterial == nullptr) {
            atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                      static_cast<unsigned long long>(1));
            break;
        }
        const auto refractiveIndex{
            SampleProperty(currentMaterial->fRindex, photon.fEnergyEv, NAN)};
        const auto velocity{SampleProperty(
            currentMaterial->fGroupVelocityMmPerNs, photon.fEnergyEv,
            isfinite(refractiveIndex) && refractiveIndex > 0.0F ? speedOfLightMmPerNs / refractiveIndex : NAN)};
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

        const auto absorptionDistance{SampleAbsorption(
            *currentMaterial, photon.fEnergyEv, gLaunchParams.fSeed,
            PhotonRandomID(photon), bounce)};
        if (absorptionDistance < distance) {
            photon.fTimeNs += absorptionDistance / velocity;
            atomicAdd(&gLaunchParams.fStats->fAbsorbedCount,
                      static_cast<unsigned long long>(1));
            break;
        }

        const auto nextPosition{Add(position, Scale(direction, distance))};
        photon.fTimeNs += distance / velocity;
        auto normal{Normalize({__uint_as_float(payload1),
                               __uint_as_float(payload2),
                               __uint_as_float(payload3)})};
        if (Dot(direction, normal) > 0.0F) {
            normal = Scale(normal, -1.0F);
        }
        const auto nextVolumeID{LocateVolume(
            Add(nextPosition,
                Scale(normal, -gLaunchParams.fBoundaryEpsilonMm)))};
        if (nextVolumeID == invalidID) {
            atomicAdd(&gLaunchParams.fStats->fEscapedCount,
                      static_cast<unsigned long long>(1));
            break;
        }

        const auto* nextSolid{FindSolid(nextVolumeID)};
        const auto* nextMaterial{
            nextSolid == nullptr ? nullptr : FindMaterial(nextSolid->fMaterialID)};
        if (nextMaterial == nullptr) {
            atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                      static_cast<unsigned long long>(1));
            break;
        }

        const auto surfaceID{FindSurfaceID(currentVolumeID, nextVolumeID)};
        const auto* surface{surfaceID < gLaunchParams.fScene.fSurfaceCount ? &gLaunchParams.fScene.fSurfaces[surfaceID] : nullptr};
        auto reflected{false};
        auto detected{false};
        auto surfaceAbsorbed{false};
        if (surface != nullptr && surface->fKind == dielectricMetal) {
            const auto reflectivity{fminf(
                fmaxf(SampleProperty(surface->fReflectivity, photon.fEnergyEv,
                                     0.0F),
                      0.0F),
                1.0F)};
            const auto efficiency{fminf(
                fmaxf(SampleProperty(surface->fEfficiency, photon.fEnergyEv,
                                     0.0F),
                      0.0F),
                1.0F)};
            const auto outcome{BoundaryPhysics::EvaluateSurface(
                efficiency, reflectivity, surface->fSensor != 0,
                Uniform(gLaunchParams.fSeed, PhotonRandomID(photon), bounce, 1U,
                        0U))};
            detected = outcome == BoundaryPhysics::SurfaceOutcome::Detect;
            reflected = outcome == BoundaryPhysics::SurfaceOutcome::Reflect;
            surfaceAbsorbed =
                outcome == BoundaryPhysics::SurfaceOutcome::Absorb;

            if (reflected && surface->fFinish == groundFinish) {
                const auto oldDirection{direction};
                direction = BoundaryPhysics::SampleLambertian(
                    normal,
                    Uniform(gLaunchParams.fSeed, PhotonRandomID(photon), bounce, 1U,
                            1U),
                    Uniform(gLaunchParams.fSeed, PhotonRandomID(photon), bounce, 1U,
                            2U));
                const auto facetNormal{
                    Normalize(Subtract(direction, oldDirection))};
                polarization = BoundaryPhysics::ReflectPolarization(
                    polarization, facetNormal, direction);
            } else if (reflected) {
                direction =
                    BoundaryPhysics::ReflectDirection(direction, normal);
                polarization = BoundaryPhysics::ReflectPolarization(
                    polarization, normal, direction);
            }
        } else {
            const auto indexTo{
                SampleProperty(nextMaterial->fRindex, photon.fEnergyEv, NAN)};
            if (!(refractiveIndex > 0.0F) ||
                !isfinite(refractiveIndex)) {
                atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                          static_cast<unsigned long long>(1));
                break;
            }
            if (!(indexTo > 0.0F) || !isfinite(indexTo)) {
                surfaceAbsorbed = true;
            } else {
                const auto fresnel{BoundaryPhysics::ComputeFresnel(
                    direction, polarization, normal, refractiveIndex,
                    indexTo)};
                reflected = Uniform(gLaunchParams.fSeed, PhotonRandomID(photon),
                                    bounce, 2U, 0U) >
                            fresnel.fTransmittance;
                if (reflected) {
                    if (surface != nullptr && surface->fFinish == groundFinish) {
                        const auto oldDirection{direction};
                        direction = BoundaryPhysics::SampleLambertian(
                            normal,
                            Uniform(gLaunchParams.fSeed,
                                    PhotonRandomID(photon), bounce, 1U, 1U),
                            Uniform(gLaunchParams.fSeed,
                                    PhotonRandomID(photon), bounce, 1U, 2U));
                        const auto facetNormal{
                            Normalize(Subtract(direction, oldDirection))};
                        polarization = BoundaryPhysics::ReflectPolarization(
                            polarization, facetNormal, direction);
                    } else {
                        direction = fresnel.fReflectedDirection;
                        polarization = fresnel.fReflectedPolarization;
                    }
                } else {
                    direction = fresnel.fTransmittedDirection;
                    polarization = fresnel.fTransmittedPolarization;
                    currentVolumeID = nextVolumeID;
                }
            }
        }

        if (detected) {
            const auto sensorID{nextSolid->fSensorID != invalidID ? nextSolid->fSensorID : currentSolid->fSensorID};
            if (sensorID == invalidID) {
                atomicAdd(&gLaunchParams.fStats->fInvalidStateCount,
                          static_cast<unsigned long long>(1));
                break;
            }
            const auto hitIndex{atomicAdd(
                &gLaunchParams.fStats->fDetectedCount,
                static_cast<unsigned long long>(1))};
            gLaunchParams.fHits[hitIndex] = {
                nextPosition,
                photon.fTimeNs,
                direction,
                photon.fEnergyEv,
                photon.fEventID,
                photon.fPhotonID,
                sensorID,
                0,
            };
            terminated = true;
            break;
        }

        if (surfaceAbsorbed) {
            atomicAdd(&gLaunchParams.fStats->fAbsorbedCount,
                      static_cast<unsigned long long>(1));
            break;
        }
        position = Add(
            nextPosition,
            Scale(normal,
                  reflected ? gLaunchParams.fBoundaryEpsilonMm : -gLaunchParams.fBoundaryEpsilonMm));
    }

    if (!terminated && bounce >= gLaunchParams.fMaxBounceCount) {
        atomicAdd(&gLaunchParams.fStats->fTruncatedCount,
                  static_cast<unsigned long long>(1));
    }
}

} // extern "C"

} // namespace G4GO::Optical
