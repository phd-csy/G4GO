#include "g4go/detector/SensorSD.hpp"

#include "G4OpticalPhoton.hh"
#include "G4Step.hh"
#include "g4go/detector/SensorHit.hpp"

namespace G4GO::Detector {

SensorSD::SensorSD(const G4String& sdname, const G4String& hcName) :
    G4VSensitiveDetector{sdname},
    fHC{nullptr},
    fHCID{-1} {
    collectionName.insert(hcName);
}

auto SensorSD::Initialize(G4HCofThisEvent* hcOfThisEvent) -> void {
    fHC = new SensorHC(SensitiveDetectorName, collectionName.at(0));
    if (fHCID < 0) {
        fHCID = GetCollectionID(0);
    }
    hcOfThisEvent->AddHitsCollection(fHCID, fHC);
}

auto SensorSD::ProcessHits(G4Step* theStep, G4TouchableHistory*) -> G4bool {
    const auto& step{*theStep};
    const auto& track{*step.GetTrack()};
    const auto& particle{*track.GetDefinition()};

    if (&particle != G4OpticalPhoton::Definition()) {
        return false;
    }

    step.GetTrack()->SetTrackStatus(fStopAndKill);

    auto hit{new SensorHit()};
    const auto* postStepPoint{step.GetPostStepPoint()};
    const auto touchable{postStepPoint->GetTouchableHandle()};
    const auto globalTime{postStepPoint->GetGlobalTime()};
    const auto copyNo{touchable->GetCopyNumber()};
    hit->GlobalTime(globalTime);
    hit->CopyNo(copyNo);
    fHC->insert(hit);

    return true;
}

} // namespace G4GO::Detector
