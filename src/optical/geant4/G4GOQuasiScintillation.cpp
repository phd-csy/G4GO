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

using Components = std::array<const G4MaterialPropertyVector*, maxComponents>;
using ComponentPhotonCounts = std::array<G4int, maxComponents>;

struct ScintillationYields {
    G4double first{};
    G4double second{};
    G4double third{};
    G4double meanPhotonCount{};
};

auto ComponentProperty(const G4MaterialPropertiesTable& properties, std::size_t componentIndex)
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

auto ComponentTimeConstant(const G4MaterialPropertiesTable& properties, std::size_t componentIndex) -> G4double {
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

auto ComponentRiseTime(const G4MaterialPropertiesTable& properties, std::size_t componentIndex) -> G4double {
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

auto RaiseSourceError(const char* method, const G4Track& track, const G4Material* material, const char* detail)
    -> void {
    G4ExceptionDescription description{};
    description << detail << " for track " << track.GetTrackID() << ", material "
                << (material == nullptr ? "<null>" : material->GetName());
    G4Exception(method, "G4GOScintillationSource", FatalException, description);
}

auto CountComponents(const G4MaterialPropertiesTable& properties, const G4Track& track, const G4Material* material)
    -> std::size_t {
    Components components{};
    std::size_t componentCount{};
    for (std::size_t i{}; i < components.size(); i++) {
        components[i] = ComponentProperty(properties, i);
        if (components.at(i) != nullptr) {
            componentCount = i + 1U;
        }
    }
    for (std::size_t i{}; i < componentCount; i++) {
        if (components.at(i) == nullptr) {
            RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track, material,
                             "scintillation components must be contiguous");
            return 0U;
        }
    }
    return componentCount;
}

auto YieldsAreValid(const ScintillationYields& yields) -> bool {
    const auto sumYields{yields.first + yields.second + yields.third};
    return std::isfinite(yields.first) and yields.first >= 0.0 and std::isfinite(yields.second) and
           yields.second >= 0.0 and std::isfinite(yields.third) and yields.third >= 0.0 and
           std::isfinite(yields.meanPhotonCount) and yields.meanPhotonCount >= 0.0 and std::isfinite(sumYields) and
           sumYields > 0.0;
}

auto SamplePhotonCount(G4double meanPhotonCount, G4double resolutionScale, const G4Track& track,
                       const G4Material* material) -> G4int {
    if (meanPhotonCount <= 10.0) {
        return G4Poisson(meanPhotonCount);
    }
    const auto sigma{resolutionScale * std::sqrt(meanPhotonCount)};
    const auto sampledCount{G4RandGauss::shoot(meanPhotonCount, sigma) + 0.5};
    if (not std::isfinite(sampledCount) or sampledCount < static_cast<G4double>(std::numeric_limits<G4int>::min()) or
        sampledCount > static_cast<G4double>(std::numeric_limits<G4int>::max())) {
        RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track, material,
                         "scintillation photon count overflow");
        return 0;
    }
    return static_cast<G4int>(sampledCount);
}

auto AllocateComponentPhotons(G4int totalPhotonCount, std::size_t componentCount, const ScintillationYields& yields)
    -> ComponentPhotonCounts {
    ComponentPhotonCounts counts{};
    if (componentCount == 1U) {
        counts.at(0) = totalPhotonCount;
        return counts;
    }
    const auto countFromYield{[totalPhotonCount](G4double yield) -> G4int {
        const auto expected{yield * totalPhotonCount};
        return static_cast<G4int>(std::clamp(expected, G4double{}, static_cast<G4double>(totalPhotonCount)));
    }};
    counts.at(0) = countFromYield(yields.first);
    if (componentCount == 2U) {
        counts.at(1) = totalPhotonCount - counts.at(0);
        return counts;
    }
    counts.at(1) = countFromYield(yields.second);
    counts.at(2) = countFromYield(yields.third);
    return counts;
}

auto CountEmissions(const ComponentPhotonCounts& counts, std::size_t componentCount) -> std::size_t {
    return static_cast<std::size_t>(std::count_if(counts.begin(),
                                                  counts.begin() + static_cast<std::ptrdiff_t>(componentCount),
                                                  [](G4int count) -> bool { return count > 0; }));
}

auto AddSecondaries(G4VParticleChange& particleChange, const G4Track& track, const G4Step& step,
                    const G4MaterialPropertiesTable& properties, const G4DynamicParticle& particle,
                    const G4QuasiOpticalData& sourceData, const ComponentPhotonCounts& counts,
                    std::size_t componentCount, bool byParticleType, bool finiteRiseTime,
                    const std::array<G4double, maxComponents>& timeConstants) -> void {
    const auto* preStepPoint{step.GetPreStepPoint()};
    const auto modelID{G4PhysicsModelCatalog::GetModelID("model_QuasiScintillation")};
    for (std::size_t i{}; i < componentCount; i++) {
        const auto photonCount{counts.at(i)};
        if (photonCount <= 0) {
            continue;
        }
        auto componentData{sourceData};
        componentData.num_photons = photonCount;
        const auto scintTime{byParticleType ? timeConstants.at(i) : ComponentTimeConstant(properties, i)};
        const auto riseTime{finiteRiseTime ? ComponentRiseTime(properties, i) : G4double{}};
        auto* quasiPhoton{
            new G4DynamicParticle(G4QuasiOpticalPhoton::QuasiOpticalPhotonDefinition(), particle.GetMomentum())};
        auto* secondary{
            new G4Track{quasiPhoton, preStepPoint->GetGlobalTime(), preStepPoint->GetPosition()}
        };
        secondary->SetTouchableHandle(preStepPoint->GetTouchableHandle());
        secondary->SetParentID(track.GetTrackID());
        secondary->SetCreatorModelID(modelID);
        secondary->SetAuxiliaryTrackInformation(
            modelID, new G4GOQuasiScintillationTrackInfo{componentData, scintTime, riseTime, static_cast<G4int>(i)});
        particleChange.AddSecondary(secondary);
    }
}

} // namespace

G4GOQuasiScintillation::G4GOQuasiScintillation(const G4String& processName, G4ProcessType type) :
    G4QuasiScintillation{processName, type} {}

// This follows Geant4's scintillation source algorithm while emitting one metadata track per component.
// Direct calls to G4VRestDiscreteProcess intentionally finalize aParticleChange without invoking
// G4QuasiScintillation's separate photon-generation implementation.
auto G4GOQuasiScintillation::PostStepDoIt(const G4Track& track, const G4Step& step) -> G4VParticleChange* {
    aParticleChange.Initialize(track);

    const auto* particle{track.GetDynamicParticle()};
    const auto* material{track.GetMaterial()};
    if (particle == nullptr or material == nullptr) {
        RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track, material,
                         "scintillation source has no particle or material");
        aParticleChange.SetNumberOfSecondaries(0);
        // NOLINTNEXTLINE(bugprone-parent-virtual-call)
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    const auto* properties{material->GetMaterialPropertiesTable()};
    if (properties == nullptr) {
        aParticleChange.SetNumberOfSecondaries(0);
        // NOLINTNEXTLINE(bugprone-parent-virtual-call)
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    const auto componentCount{CountComponents(*properties, track, material)};
    if (componentCount == 0U) {
        aParticleChange.SetNumberOfSecondaries(0);
        // NOLINTNEXTLINE(bugprone-parent-virtual-call)
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    const auto resolutionScale{properties->ConstPropertyExists(kRESOLUTIONSCALE) ?
                                   properties->GetConstProperty(kRESOLUTIONSCALE) :
                                   G4double{1.0}};
    if (not std::isfinite(resolutionScale) or resolutionScale < 0.0) {
        RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track, material,
                         "invalid scintillation resolution scale");
        aParticleChange.SetNumberOfSecondaries(0);
        // NOLINTNEXTLINE(bugprone-parent-virtual-call)
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    ScintillationYields yields{};
    G4double timeConstant1{};
    G4double timeConstant2{};
    G4double timeConstant3{};
    const auto totalEnergyDeposit{step.GetTotalEnergyDeposit()};
    if (GetScintillationByParticleType()) {
        yields.meanPhotonCount = GetScintillationYieldByParticleType(
            track, step, yields.first, yields.second, yields.third, timeConstant1, timeConstant2, timeConstant3);
    } else {
        yields.first = properties->ConstPropertyExists(kSCINTILLATIONYIELD1) ?
                           properties->GetConstProperty(kSCINTILLATIONYIELD1) :
                           G4double{1.0};
        yields.second = properties->ConstPropertyExists(kSCINTILLATIONYIELD2) ?
                            properties->GetConstProperty(kSCINTILLATIONYIELD2) :
                            G4double{};
        yields.third = properties->ConstPropertyExists(kSCINTILLATIONYIELD3) ?
                           properties->GetConstProperty(kSCINTILLATIONYIELD3) :
                           G4double{};
        yields.meanPhotonCount = properties->GetConstProperty(kSCINTILLATIONYIELD);
        if (GetSaturation() != nullptr) {
            yields.meanPhotonCount *= GetSaturation()->VisibleEnergyDepositionAtAStep(&step);
        } else {
            yields.meanPhotonCount *= totalEnergyDeposit;
        }
    }

    if (not YieldsAreValid(yields)) {
        RaiseSourceError("G4GOQuasiScintillation::PostStepDoIt", track, material, "invalid scintillation yield");
        aParticleChange.SetNumberOfSecondaries(0);
        // NOLINTNEXTLINE(bugprone-parent-virtual-call)
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    const auto totalPhotonCount{SamplePhotonCount(yields.meanPhotonCount, resolutionScale, track, material)};
    if (totalPhotonCount <= 0) {
        aParticleChange.SetNumberOfSecondaries(0);
        // NOLINTNEXTLINE(bugprone-parent-virtual-call)
        return G4VRestDiscreteProcess::PostStepDoIt(track, step);
    }

    const auto componentPhotonCounts{AllocateComponentPhotons(totalPhotonCount, componentCount, yields)};
    const auto emissionCount{CountEmissions(componentPhotonCounts, componentCount)};
    aParticleChange.SetNumberOfSecondaries(static_cast<G4int>(emissionCount));
    if (GetTrackSecondariesFirst() and track.GetTrackStatus() == fAlive) {
        aParticleChange.ProposeTrackStatus(fSuspend);
    }

    const auto deltaVelocity{step.GetPostStepPoint()->GetVelocity() - step.GetPreStepPoint()->GetVelocity()};
    const G4QuasiOpticalData sourceData{material->GetIndex(),
                                        0,
                                        particle->GetDefinition()->GetPDGCharge(),
                                        step.GetStepLength(),
                                        step.GetPreStepPoint()->GetVelocity(),
                                        deltaVelocity,
                                        step.GetDeltaPosition()};

    AddSecondaries(aParticleChange, track, step, *properties, *particle, sourceData, componentPhotonCounts,
                   componentCount, GetScintillationByParticleType(), GetFiniteRiseTime(),
                   {timeConstant1, timeConstant2, timeConstant3});

    // NOLINTNEXTLINE(bugprone-parent-virtual-call)
    return G4VRestDiscreteProcess::PostStepDoIt(track, step);
}

auto G4GOQuasiScintillation::AtRestDoIt(const G4Track& track, const G4Step& step) -> G4VParticleChange* {
    return PostStepDoIt(track, step);
}
