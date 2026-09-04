// ********************************************************************
// * License and Disclaimer                                           *
// *                                                                  *
// * The Geant4 software is copyright of the Copyright Holders of     *
// * Geant4.  It is provided under the Geant4 Software License.       *
// ********************************************************************

#include "g4go/optical/geant4/G4GOQuasiCerenkov.hpp"

#include "G4Material.hh"
#include "G4MaterialPropertiesTable.hh"
#include "G4MaterialPropertyVector.hh"
#include "G4PhysicalConstants.hh"
#include "G4Step.hh"
#include "G4StepPoint.hh"
#include "G4Track.hh"
#include "Randomize.hh"

#include <algorithm>
#include <cmath>

namespace {

auto ConsumeNativeCerenkovDraws(const G4GOQuasiCerenkov& process,
                                const G4Track& track,
                                const G4Step& step,
                                G4int photonCount) -> void {
    if (photonCount <= 0) {
        return;
    }
    const auto* particle{track.GetDynamicParticle()};
    const auto* material{track.GetMaterial()};
    if (particle == nullptr || material == nullptr) {
        return;
    }
    auto* properties{material->GetMaterialPropertiesTable()};
    auto* rindex{properties == nullptr ?
                     nullptr :
                     properties->GetProperty(kRINDEX)};
    if (rindex == nullptr) {
        return;
    }

    const auto charge{particle->GetDefinition()->GetPDGCharge()};
    const auto* preStepPoint{step.GetPreStepPoint()};
    const auto* postStepPoint{step.GetPostStepPoint()};
    if (preStepPoint == nullptr || postStepPoint == nullptr) {
        return;
    }
    const auto beta1{preStepPoint->GetBeta()};
    const auto beta2{postStepPoint->GetBeta()};
    const auto beta{(beta1 + beta2) * 0.5};
    if (!(beta > 0.0) || !std::isfinite(beta)) {
        return;
    }

    // These values and rejection loops mirror G4QuasiCerenkov's regular
    // secondary loop.  The compact path has already sampled the Poisson
    // count; consuming the remaining draws preserves the charged-particle
    // source stream for subsequent Geant4 transport.
    const auto meanNumberOfPhotons{
        process.GetAverageNumberOfPhotons(charge, beta, material, rindex)};
    const auto meanNumberOfPhotons1{
        process.GetAverageNumberOfPhotons(charge, beta1, material, rindex)};
    const auto meanNumberOfPhotons2{
        process.GetAverageNumberOfPhotons(charge, beta2, material, rindex)};
    if (!(meanNumberOfPhotons > 0.0) ||
        !(std::max(meanNumberOfPhotons1, meanNumberOfPhotons2) >= 1e-15) ||
        !std::isfinite(meanNumberOfPhotons1) ||
        !std::isfinite(meanNumberOfPhotons2)) {
        return;
    }

    const auto pMin{rindex->Energy(0)};
    const auto pMax{rindex->GetMaxEnergy()};
    const auto dp{pMax - pMin};
    const auto deltaNumberOfPhotons{
        meanNumberOfPhotons1 - meanNumberOfPhotons2};
    const auto maxNumberOfPhotons{
        std::max(meanNumberOfPhotons1, meanNumberOfPhotons2)};
    const auto nMax{rindex->GetMaxValue()};
    const auto betaInverse{1.0 / beta};
    const auto maxCos{betaInverse / nMax};
    const auto maxSin2{1.0 - maxCos * maxCos};
    if (!std::isfinite(pMin) || !std::isfinite(pMax) ||
        !std::isfinite(dp) || dp < 0.0 || !(nMax > 0.0) ||
        !std::isfinite(nMax) || !std::isfinite(maxSin2) ||
        maxSin2 < 0.0 || !std::isfinite(maxNumberOfPhotons) ||
        maxNumberOfPhotons < 0.0) {
        return;
    }

    for (auto photon{G4int{}}; photon < photonCount; ++photon) {
        G4double random{};
        G4double sampledEnergy{};
        G4double sin2Theta{};
        do {
            random = G4UniformRand();
            sampledEnergy = pMin + random * dp;
            const auto refractiveIndex{rindex->Value(sampledEnergy)};
            if (!(refractiveIndex > 0.0) ||
                !std::isfinite(refractiveIndex)) {
                return;
            }
            const auto cosTheta{betaInverse / refractiveIndex};
            sin2Theta = 1.0 - cosTheta * cosTheta;
            random = G4UniformRand();
        } while (random * maxSin2 > sin2Theta);

        // Azimuth of the photon momentum.
        static_cast<void>(G4UniformRand());

        // Position/time acceptance uses the same random value after the
        // rejection loop; only the acceptance draw itself is consumed here.
        do {
            random = G4UniformRand();
        } while (G4UniformRand() * maxNumberOfPhotons >
                 meanNumberOfPhotons1 - random * deltaNumberOfPhotons);
    }
}

} // namespace

G4GOQuasiCerenkov::G4GOQuasiCerenkov(const G4String& processName,
                                     G4ProcessType type) :
    G4QuasiCerenkov{processName, type} {}

auto G4GOQuasiCerenkov::PostStepDoIt(const G4Track& track,
                                     const G4Step& step)
    -> G4VParticleChange* {
    auto* particleChange{G4QuasiCerenkov::PostStepDoIt(track, step)};
    if (GetOffloadPhotons() && GetStackPhotons() && GetNumPhotons() > 0) {
        ConsumeNativeCerenkovDraws(*this, track, step, GetNumPhotons());
    }
    return particleChange;
}
