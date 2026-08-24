#include "g4go/optical/cpu/Transport.hpp"

#include "g4go/optical/Geometry.hpp"
#include "g4go/optical/Random.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

namespace G4GO::Optical {

namespace {

constexpr auto speedOfLightMmPerNs{299.792458F};
constexpr auto invalidFloat{std::numeric_limits<float>::quiet_NaN()};

auto Add(Vector3 left, Vector3 right) -> Vector3 {
    return {left.fX + right.fX, left.fY + right.fY, left.fZ + right.fZ};
}

auto Scale(Vector3 vector, float value) -> Vector3 {
    return {vector.fX * value, vector.fY * value, vector.fZ * value};
}

auto Subtract(Vector3 left, Vector3 right) -> Vector3 {
    return {left.fX - right.fX, left.fY - right.fY, left.fZ - right.fZ};
}

auto Dot(Vector3 left, Vector3 right) -> float {
    return left.fX * right.fX + left.fY * right.fY + left.fZ * right.fZ;
}

auto LengthSquared(Vector3 vector) -> float { return Dot(vector, vector); }

auto Normalize(Vector3 vector) -> Vector3 {
    const auto lengthSquared{LengthSquared(vector)};
    if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0F) {
        return {};
    }
    return Scale(vector, 1.0F / std::sqrt(lengthSquared));
}

auto Reflect(Vector3 direction, Vector3 normal) -> Vector3 {
    return Normalize(Subtract(direction,
                              Scale(normal, 2.0F * Dot(direction, normal))));
}

auto ProjectPolarization(Vector3 polarization, Vector3 direction) -> Vector3 {
    auto result{
        Subtract(polarization, Scale(direction, Dot(polarization, direction)))};
    if (LengthSquared(result) < 1.0e-12F) {
        const auto helper{
            std::abs(direction.fZ) < 0.9F ? Vector3{0.0F, 0.0F, 1.0F}
                : Vector3{1.0F, 0.0F, 0.0F}
        };
        result = {
            direction.fY * helper.fZ - direction.fZ * helper.fY,
            direction.fZ * helper.fX - direction.fX * helper.fZ,
            direction.fX * helper.fY - direction.fY * helper.fX,
        };
    }
    return Normalize(result);
}

auto SampleAbsorption(const Material& material,
                      float energyEv,
                      std::uint64_t seed,
                      std::uint64_t photonID,
                      std::uint32_t bounce) -> float {
    const auto absorptionLength{
        material.fAbsLengthMm.Sample(
            energyEv, std::numeric_limits<float>::infinity())};
    if (!std::isfinite(absorptionLength) || absorptionLength <= 0.0F) {
        return std::numeric_limits<float>::infinity();
    }
    const auto random{Uniform(seed, photonID, bounce, 0U, 0U)};
    return -absorptionLength * std::log(random);
}

auto GroupVelocity(const Material& material, float energyEv) -> float {
    const auto velocity{material.fGroupVelocityMmPerNs.Sample(
        energyEv, std::numeric_limits<float>::quiet_NaN())};
    if (std::isfinite(velocity) && velocity > 0.0F) {
        return velocity;
    }
    const auto refractiveIndex{material.fRindex.Sample(energyEv, invalidFloat)};
    if (!std::isfinite(refractiveIndex) || refractiveIndex <= 0.0F) {
        return invalidFloat;
    }
    return speedOfLightMmPerNs / refractiveIndex;
}

struct FresnelResult {
    float reflectance{};
    float cosineTransmitted{};
    float eta{};
};

auto CalculateFresnel(float indexFrom,
                      float indexTo,
                      float cosineIncident) -> FresnelResult {
    const auto eta{indexFrom / indexTo};
    const auto sineTransmittedSquared{
        eta * eta * std::max(0.0F, 1.0F - cosineIncident * cosineIncident)};
    if (sineTransmittedSquared >= 1.0F) {
        return {1.0F, 0.0F, eta};
    }

    const auto cosineTransmitted{
        std::sqrt(std::max(0.0F, 1.0F - sineTransmittedSquared))};
    const auto rs{(indexFrom * cosineIncident - indexTo * cosineTransmitted) /
                  (indexFrom * cosineIncident + indexTo * cosineTransmitted)};
    const auto rp{(indexTo * cosineIncident - indexFrom * cosineTransmitted) /
                  (indexTo * cosineIncident + indexFrom * cosineTransmitted)};
    return {0.5F * (rs * rs + rp * rp), cosineTransmitted, eta};
}

auto Refract(Vector3 direction,
             Vector3 normal,
             float eta,
             float cosineIncident,
             float cosineTransmitted) -> Vector3 {
    return Normalize(Add(Scale(direction, eta),
                         Scale(normal, eta * cosineIncident -
                                           cosineTransmitted)));
}

auto PhotonPosition(const Photon& photon) -> Vector3 {
    return {photon.fPositionMm.fX, photon.fPositionMm.fY,
            photon.fPositionMm.fZ};
}

auto PhotonDirection(const Photon& photon) -> Vector3 {
    return {photon.fDirection.fX, photon.fDirection.fY,
            photon.fDirection.fZ};
}

auto PhotonPolarization(const Photon& photon) -> Vector3 {
    return {photon.fPolarization.fX, photon.fPolarization.fY,
            photon.fPolarization.fZ};
}

auto SetPhotonPosition(Photon& photon, Vector3 position) -> void {
    photon.fPositionMm = position;
}

auto SetPhotonDirection(Photon& photon, Vector3 direction) -> void {
    photon.fDirection = direction;
}

auto SetPhotonPolarization(Photon& photon, Vector3 polarization) -> void {
    photon.fPolarization = polarization;
}

auto MakeHit(const Photon& photon, Vector3 position, std::uint32_t sensorID)
    -> PhotonHit {
    return {
        {position.fX, position.fY, position.fZ},
        photon.fTimeNs,
        PhotonDirection(photon),
        photon.fEnergyEv,
        photon.fPhotonID,
        sensorID,
        0,
    };
}

auto IsFinite(const Photon& photon) -> bool {
    const std::array values{
        photon.fPositionMm.fX,
        photon.fPositionMm.fY,
        photon.fPositionMm.fZ,
        photon.fTimeNs,
        photon.fDirection.fX,
        photon.fDirection.fY,
        photon.fDirection.fZ,
        photon.fEnergyEv,
    };
    return std::all_of(values.begin(), values.end(),
                       [](auto value) { return std::isfinite(value); });
}

} // namespace

CpuOpticalTransport::CpuOpticalTransport(TransportConfig config) :
    fConfig{config} {}

auto CpuOpticalTransport::Transport(const Scene& scene,
                                    std::span<const Photon> photonData)
    -> TransportResult {
    TransportResult result{};
    result.fStats.fCapturedCount = photonData.size();
    result.fHitData.reserve(photonData.size());
    const auto start{std::chrono::steady_clock::now()};

    for (const auto& input : photonData) {
        auto photon{input};
        auto position{PhotonPosition(photon)};
        auto direction{Normalize(PhotonDirection(photon))};
        auto polarization{ProjectPolarization(PhotonPolarization(photon),
                                              direction)};
        SetPhotonDirection(photon, direction);
        SetPhotonPolarization(photon, polarization);

        if (!IsFinite(photon) ||
            scene.FindSolid(photon.fVolumeID) == nullptr) {
            ++result.fStats.fInvalidStateCount;
            continue;
        }

        auto terminated{false};
        std::uint32_t bounce{};
        for (; bounce < fConfig.fMaxBounceCount && !terminated; ++bounce) {
            result.fStats.fMaxBounceCount =
                std::max<std::uint64_t>(result.fStats.fMaxBounceCount, bounce);
            const auto* currentSolid{scene.FindSolid(photon.fVolumeID)};
            if (currentSolid == nullptr) {
                ++result.fStats.fInvalidStateCount;
                break;
            }
            const auto* currentMaterial{
                scene.FindMaterial(currentSolid->fMaterialID)};
            if (currentMaterial == nullptr) {
                ++result.fStats.fInvalidStateCount;
                break;
            }

            const auto velocity{GroupVelocity(*currentMaterial,
                                              photon.fEnergyEv)};
            if (!std::isfinite(velocity) || velocity <= 0.0F) {
                ++result.fStats.fInvalidStateCount;
                break;
            }

            const auto intersection{
                NextIntersection(scene, photon, fConfig.fBoundaryEpsilonMm)};
            if (!intersection) {
                ++result.fStats.fEscapedCount;
                break;
            }

            const auto absorptionDistance{SampleAbsorption(
                *currentMaterial, photon.fEnergyEv, fConfig.fSeed,
                photon.fPhotonID, bounce)};
            if (absorptionDistance < intersection->fDistanceMm) {
                position = Add(position, Scale(direction, absorptionDistance));
                photon.fTimeNs += absorptionDistance / velocity;
                ++result.fStats.fAbsorbedCount;
                break;
            }

            position = Add(position,
                           Scale(direction, intersection->fDistanceMm));
            photon.fTimeNs += intersection->fDistanceMm / velocity;
            const auto forwardPosition{Add(
                position, Scale(direction, fConfig.fBoundaryEpsilonMm))};
            const auto nextVolumeID{LocateVolume(scene, forwardPosition)};
            if (nextVolumeID == InvalidID) {
                ++result.fStats.fEscapedCount;
                break;
            }

            auto normal{Normalize(intersection->fNormal)};
            if (Dot(direction, normal) > 0.0F) {
                normal = Scale(normal, -1.0F);
            }
            const auto* nextSolid{scene.FindSolid(nextVolumeID)};
            const auto* nextMaterial{
                nextSolid == nullptr ? nullptr : scene.FindMaterial(nextSolid->fMaterialID)};
            if (nextMaterial == nullptr) {
                ++result.fStats.fInvalidStateCount;
                break;
            }

            const auto* surface{
                scene.FindBoundarySurface(photon.fVolumeID, nextVolumeID)};
            auto reflected{false};
            auto detected{false};
            if (surface != nullptr &&
                surface->fKind == SurfaceKind::DielectricMetal) {
                const auto reflectivity{std::clamp(
                    surface->fReflectivity.Sample(photon.fEnergyEv, 0.0F),
                    0.0F, 1.0F)};
                if (Uniform(fConfig.fSeed, photon.fPhotonID, bounce, 1U, 0U) <
                    reflectivity) {
                    reflected = true;
                } else {
                    const auto efficiency{std::clamp(
                        surface->fEfficiency.Sample(photon.fEnergyEv, 0.0F),
                        0.0F, 1.0F)};
                    detected =
                        Uniform(fConfig.fSeed, photon.fPhotonID, bounce, 1U,
                                1U) < efficiency;
                }
            } else {
                const auto indexFrom{
                    currentMaterial->fRindex.Sample(photon.fEnergyEv,
                                                    invalidFloat)};
                const auto indexTo{
                    nextMaterial->fRindex.Sample(photon.fEnergyEv, invalidFloat)};
                const auto cosineIncident{
                    std::clamp(-Dot(direction, normal), 0.0F, 1.0F)};
                if (!std::isfinite(indexFrom) || !std::isfinite(indexTo) ||
                    indexFrom <= 0.0F || indexTo <= 0.0F) {
                    ++result.fStats.fInvalidStateCount;
                    break;
                }
                const auto fresnel{
                    CalculateFresnel(indexFrom, indexTo, cosineIncident)};
                reflected =
                    Uniform(fConfig.fSeed, photon.fPhotonID, bounce, 2U, 0U) <
                    fresnel.reflectance;
                if (!reflected) {
                    direction = Refract(direction, normal, fresnel.eta,
                                        cosineIncident,
                                        fresnel.cosineTransmitted);
                    polarization =
                        ProjectPolarization(polarization, direction);
                }
            }

            if (detected) {
                const auto sensorID{nextSolid->fSensorID != InvalidID ? nextSolid->fSensorID : currentSolid->fSensorID};
                if (sensorID == InvalidID) {
                    ++result.fStats.fInvalidStateCount;
                    break;
                }
                result.fHitData.push_back(MakeHit(photon, position, sensorID));
                ++result.fStats.fDetectedCount;
                terminated = true;
                break;
            }

            if (surface != nullptr &&
                surface->fKind == SurfaceKind::DielectricMetal &&
                !reflected) {
                ++result.fStats.fAbsorbedCount;
                break;
            }

            if (reflected) {
                direction = Reflect(direction, normal);
                polarization = ProjectPolarization(polarization, direction);
            } else {
                photon.fVolumeID = nextVolumeID;
            }
            position = Add(position,
                           Scale(direction, fConfig.fBoundaryEpsilonMm));
            SetPhotonDirection(photon, direction);
            SetPhotonPolarization(photon, polarization);
            SetPhotonPosition(photon, position);
        }

        if (!terminated && bounce >= fConfig.fMaxBounceCount) {
            ++result.fStats.fAbsorbedCount;
        }
    }

    result.fStats.fTransportTimeMs =
        std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - start}
            .count();
    return result;
}

} // namespace G4GO::Optical
