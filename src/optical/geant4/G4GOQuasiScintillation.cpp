// ********************************************************************
// * License and Disclaimer                                           *
// *                                                                  *
// * The  Geant4 software is  copyright of the Copyright Holders  of *
// * the  Geant4 Collaboration.  It is provided  under  the  terms  *
// * and  conditions  of  the  Geant4 Software License,  included in *
// * the file  LICENSE  and available at  http://cern.ch/geant4/license .  *
// * These include a list of copyright holders.                       *
// *                                                                  *
// * Neither the authors of this software system, nor their employing *
// * institutes,nor the agencies providing financial support for this *
// * work  make  any representation or  warranty, express or  implied, *
// * regarding  this  software system or assume any liability for its *
// * use.  Please see the license in the file  LICENSE  and URL above *
// * for the full disclaimer and the limitation of liability.         *
// *                                                                  *
// * This  code  implementation is the result of  the  scientific and *
// * technical work of the GEANT4 collaboration.                      *
// * By using,  copying,  modifying or  distributing the software (or *
// * any work based  on the software)  you  agree  to acknowledge its *
// * use  in  resulting  scientific  publications,  and indicate your *
// * acceptance of all terms of the Geant4 Software license.          *
// ********************************************************************
//
// This implementation follows Geant4 11.4.2 G4QuasiScintillation.cc:
// https://github.com/Geant4/geant4/blob/v11.4.2/source/processes/electromagnetic/xrays/src/G4QuasiScintillation.cc
// The upstream Geant4 implementation is distributed under the Geant4
// Software License. G4GO keeps the source-calculation semantics while
// replacing regular optical-photon creation with compact quasi tracks.
//

#include "g4go/optical/geant4/G4GOQuasiScintillation.hpp"

#include "G4DynamicParticle.hh"
#include "G4Exception.hh"
#include "G4Material.hh"
#include "G4MaterialPropertiesTable.hh"
#include "G4MaterialPropertyVector.hh"
#include "G4PhysicsModelCatalog.hh"
#include "G4Poisson.hh"
#include "G4QuasiOpticalData.hh"
#include "G4QuasiOpticalPhoton.hh"
#include "G4Step.hh"
#include "G4StepPoint.hh"
#include "G4SystemOfUnits.hh"
#include "G4Track.hh"
#include "G4VParticleChange.hh"
#include "Randomize.hh"
#include "g4go/optical/geant4/G4GOQuasiScintillationTrackInfo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace {

constexpr std::size_t maxComponents{3};

auto ComponentProperty(const G4MaterialPropertiesTable& properties,
                       std::size_t componentIndex)
    -> const G4MaterialPropertyVector* {
    switch (componentIndex) {
    case 0:
        return properties.GetProperty(kSCINTILLATIONCOMPONENT1);
    case 1:
        return properties.GetProperty(kSCINTILLATIONCOMPONENT2);
    case 2:
        return properties.GetProperty(kSCINTILLATIONCOMPONENT3);
    default:
        return nullptr;
    }
}

auto ComponentTimeConstant(const G4MaterialPropertiesTable& properties,
                           std::size_t componentIndex) -> G4double {
    switch (componentIndex) {
    case 0:
        return properties.GetConstProperty(kSCINTILLATIONTIMECONSTANT1);
    case 1:
        return properties.GetConstProperty(kSCINTILLATIONTIMECONSTANT2);
    case 2:
        return properties.GetConstProperty(kSCINTILLATIONTIMECONSTANT3);
    default:
        return 0.0;
    }
}

auto ComponentRiseTime(const G4MaterialPropertiesTable& properties,
                       std::size_t componentIndex) -> G4double {
    switch (componentIndex) {
    case 0:
        return properties.GetConstProperty(kSCINTILLATIONRISETIME1);
    case 1:
        return properties.GetConstProperty(kSCINTILLATIONRISETIME2);
    case 2:
        return properties.GetConstProperty(kSCINTILLATIONRISETIME3);
    default:
        return 0.0;
    }
}

auto RaiseSourceError(const char* method,
                      const G4Track& track,
                      const G4Material* material,
                      const char* detail) -> void {
    G4ExceptionDescription description{};
    description << detail << " for track " << track.GetTrackID()
                << ", material "
                << (material == nullptr ? "<null>" : material->GetName());
    G4Exception(method, "G4GOScintillationSource", FatalException,
                description);
}

} // namespace

G4GOQuasiScintillation::G4GOQuasiScintillation(
    const G4String& processName, G4ProcessType type) :
    G4QuasiScintillation{processName, type} {}

auto G4GOQuasiScintillation::PostStepDoIt(const G4Track& track,
                                          const G4Step& step)
    -> G4VParticleChange* {
    aParticleChange.Initialize(track);

    // This process is registered only for the GPU source path. Returning an
    // unchanged particle keeps a misconfigured instance from entering the
    // regular Geant4 optical-photon secondary loop.
    if (!GetOffloadPhotons() || !GetStackPhotons()) {
        aParticleChange.SetNumberOfSecondaries(0);
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    const auto* particle{track.GetDynamicParticle()};
    const auto* material{track.GetMaterial()};
    if (particle == nullptr || material == nullptr) {
        RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track,
                         material, "scintillation source has no particle or material");
        aParticleChange.SetNumberOfSecondaries(0);
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    const auto* properties{material->GetMaterialPropertiesTable()};
    if (properties == nullptr) {
        aParticleChange.SetNumberOfSecondaries(0);
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    std::array<const G4MaterialPropertyVector*, maxComponents> components{};
    std::size_t componentCount{};
    for (std::size_t index{}; index < components.size(); ++index) {
        components[index] = ComponentProperty(*properties, index);
        if (components.at(index) != nullptr) {
            componentCount = index + 1U;
        }
    }
    if (componentCount == 0U) {
        aParticleChange.SetNumberOfSecondaries(0);
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }
    for (std::size_t index{}; index < componentCount; ++index) {
        if (components.at(index) == nullptr) {
            RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track,
                             material,
                             "scintillation components must be contiguous");
            aParticleChange.SetNumberOfSecondaries(0);
            return G4VRestDiscreteProcess::PostStepDoIt(track, step);
        }
    }

    const auto resolutionScale{
        properties->ConstPropertyExists(kRESOLUTIONSCALE) ?
            properties->GetConstProperty(kRESOLUTIONSCALE) :
            G4double{1.0}};
    if (!std::isfinite(resolutionScale) || resolutionScale < 0.0) {
        RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track,
                         material, "invalid scintillation resolution scale");
        aParticleChange.SetNumberOfSecondaries(0);
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    G4double yield1{};
    G4double yield2{};
    G4double yield3{};
    G4double timeConstant1{};
    G4double timeConstant2{};
    G4double timeConstant3{};
    G4double meanNumberOfPhotons{};
    const auto totalEnergyDeposit{step.GetTotalEnergyDeposit()};
    if (GetScintillationByParticleType()) {
        meanNumberOfPhotons = GetScintillationYieldByParticleType(
            track, step, yield1, yield2, yield3, timeConstant1,
            timeConstant2, timeConstant3);
    } else {
        yield1 = properties->ConstPropertyExists(kSCINTILLATIONYIELD1) ?
                     properties->GetConstProperty(kSCINTILLATIONYIELD1) :
                     G4double{1.0};
        yield2 = properties->ConstPropertyExists(kSCINTILLATIONYIELD2) ?
                     properties->GetConstProperty(kSCINTILLATIONYIELD2) :
                     G4double{};
        yield3 = properties->ConstPropertyExists(kSCINTILLATIONYIELD3) ?
                     properties->GetConstProperty(kSCINTILLATIONYIELD3) :
                     G4double{};
        meanNumberOfPhotons = properties->GetConstProperty(kSCINTILLATIONYIELD);
        if (GetSaturation() != nullptr) {
            meanNumberOfPhotons *=
                GetSaturation()->VisibleEnergyDepositionAtAStep(&step);
        } else {
            meanNumberOfPhotons *= totalEnergyDeposit;
        }
    }

    const auto sumYields{yield1 + yield2 + yield3};
    if (!std::isfinite(yield1) || yield1 < 0.0 ||
        !std::isfinite(yield2) || yield2 < 0.0 ||
        !std::isfinite(yield3) || yield3 < 0.0 ||
        !std::isfinite(meanNumberOfPhotons) || meanNumberOfPhotons < 0.0 ||
        !std::isfinite(sumYields) || sumYields <= 0.0) {
        RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track,
                         material, "invalid scintillation yield");
        aParticleChange.SetNumberOfSecondaries(0);
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    G4int totalPhotonCount{};
    if (meanNumberOfPhotons > 10.0) {
        const auto sigma{resolutionScale * std::sqrt(meanNumberOfPhotons)};
        const auto sampledCount{
            G4RandGauss::shoot(meanNumberOfPhotons, sigma) + 0.5};
        if (!std::isfinite(sampledCount) ||
            sampledCount <
                static_cast<G4double>(std::numeric_limits<G4int>::min()) ||
            sampledCount >
                static_cast<G4double>(std::numeric_limits<G4int>::max())) {
            RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track,
                             material, "scintillation photon count overflow");
            aParticleChange.SetNumberOfSecondaries(0);
            return G4VRestDiscreteProcess::PostStepDoIt(track, step);
        }
        totalPhotonCount = static_cast<G4int>(sampledCount);
    } else {
        totalPhotonCount = G4Poisson(meanNumberOfPhotons);
    }
    if (totalPhotonCount <= 0) {
        aParticleChange.SetNumberOfSecondaries(0);
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    std::array<G4int, maxComponents> componentPhotonCounts{};
    if (componentCount == 1U) {
        componentPhotonCounts.at(0) = totalPhotonCount;
    } else {
        const auto countFromYield{[&](G4double yield) {
            const auto expected{yield * totalPhotonCount};
            return static_cast<G4int>(std::clamp(
                expected, G4double{},
                static_cast<G4double>(totalPhotonCount)));
        }};
        componentPhotonCounts.at(0) = countFromYield(yield1);
        if (componentCount == 2U) {
            componentPhotonCounts.at(1) =
                totalPhotonCount - componentPhotonCounts.at(0);
        } else {
            componentPhotonCounts.at(1) = countFromYield(yield2);
            componentPhotonCounts.at(2) = countFromYield(yield3);
        }
    }

    std::size_t emissionCount{};
    for (std::size_t index{}; index < componentCount; ++index) {
        if (componentPhotonCounts.at(index) > 0) {
            ++emissionCount;
        }
    }
    aParticleChange.SetNumberOfSecondaries(
        static_cast<G4int>(emissionCount));
    if (GetTrackSecondariesFirst() && track.GetTrackStatus() == fAlive) {
        aParticleChange.ProposeTrackStatus(fSuspend);
    }

    const auto stepPosition{step.GetPreStepPoint()->GetPosition()};
    const auto initialTime{step.GetPreStepPoint()->GetGlobalTime()};
    const auto deltaVelocity{step.GetPostStepPoint()->GetVelocity() -
                             step.GetPreStepPoint()->GetVelocity()};
    const auto touchableHandle{step.GetPreStepPoint()->GetTouchableHandle()};
    const auto modelID{
        G4PhysicsModelCatalog::GetModelID("model_QuasiScintillation")};
    const G4QuasiOpticalData sourceData{
        material->GetIndex(),
        0,
        particle->GetDefinition()->GetPDGCharge(),
        step.GetStepLength(),
        step.GetPreStepPoint()->GetVelocity(),
        deltaVelocity,
        step.GetDeltaPosition()};

    for (std::size_t index{}; index < componentCount; ++index) {
        const auto photonCount{componentPhotonCounts.at(index)};
        if (photonCount <= 0) {
            continue;
        }
        auto componentData{sourceData};
        componentData.num_photons = photonCount;
        const auto scintTime{GetScintillationByParticleType() ?
                                 (index == 0U ? timeConstant1 :
                                  index == 1U ? timeConstant2 :
                                                timeConstant3) :
                                 ComponentTimeConstant(*properties, index)};
        const auto riseTime{GetFiniteRiseTime() ?
                                ComponentRiseTime(*properties, index) :
                                G4double{}};
        auto* quasiPhoton{new G4DynamicParticle(
            G4QuasiOpticalPhoton::QuasiOpticalPhotonDefinition(),
            particle->GetMomentum())};
        auto* secondary{
            new G4Track{quasiPhoton, initialTime, stepPosition}
        };
        secondary->SetTouchableHandle(touchableHandle);
        secondary->SetParentID(track.GetTrackID());
        secondary->SetCreatorModelID(modelID);
        secondary->SetAuxiliaryTrackInformation(
            modelID, new G4GOQuasiScintillationTrackInfo{
                         componentData, scintTime, riseTime,
                         static_cast<G4int>(index)});
        aParticleChange.AddSecondary(secondary);
    }

    return G4VRestDiscreteProcess::PostStepDoIt(track, step);
}

auto G4GOQuasiScintillation::AtRestDoIt(const G4Track& track,
                                        const G4Step& step)
    -> G4VParticleChange* {
    return PostStepDoIt(track, step);
}
