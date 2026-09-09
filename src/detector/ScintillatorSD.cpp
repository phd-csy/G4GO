#include "g4go/detector/ScintillatorSD.hpp"

#include "G4OpticalPhoton.hh"
#include "G4RunManager.hh"
#include "G4Step.hh"
#include "g4go/detector/DetectorConstruction.hpp"
#include "g4go/detector/ScintillatorHit.hpp"

namespace G4GO::Detector {

ScintillatorSD::ScintillatorSD(const G4String& sdName, const G4String& hcName) :
    G4VSensitiveDetector{sdName},
    fHitsCollection{nullptr},
    fHitsCollectionID{-1} {
    collectionName.insert(hcName);
}

auto ScintillatorSD::Initialize(G4HCofThisEvent* hcOfThisEvent) -> void {
    const auto* detectorConstruction{
        static_cast<const DetectorConstruction*>(G4RunManager::GetRunManager()->GetUserDetectorConstruction())};
    const auto moduleID{detectorConstruction->ModuleID()};

    fHitsCollection = new ScintillatorHC(SensitiveDetectorName, collectionName.at(0));
    if (fHitsCollectionID < 0) {
        fHitsCollectionID = GetCollectionID(0);
    }
    hcOfThisEvent->AddHitsCollection(fHitsCollectionID, fHitsCollection);
    for (int i{}; i < moduleID; ++i) {
        fHitsCollection->insert(new ScintillatorHit());
    }
}

auto ScintillatorSD::ProcessHits(G4Step* step, G4TouchableHistory*) -> G4bool {
    auto particleDefinition{step->GetTrack()->GetDefinition()};
    if (particleDefinition != G4OpticalPhoton::OpticalPhotonDefinition()) {
        const auto touchable{step->GetPreStepPoint()->GetTouchableHandle()};
        const auto copyNo{touchable->GetCopyNumber()};
        fHitsCollection->GetVector()->at(copyNo)->AddEnergyDeposit(step->GetTotalEnergyDeposit());
    }
    return true;
}

} // namespace G4GO::Detector
