#include "g4go/simulation/EventAction.hpp"

#include "G4AnalysisManager.hh"
#include "G4Event.hh"
#include "G4RunManager.hh"
#include "G4SDManager.hh"
#include "G4SystemOfUnits.hh"
#include "g4go/detector/DetectorConstruction.hpp"
#include "g4go/detector/ScintillatorHit.hpp"
#include "g4go/detector/SensorHit.hpp"
#include "g4go/optical/geant4/EventBridge.hpp"
#include "g4go/simulation/EventOutput.hpp"

#include <utility>

namespace G4GO::Simulation {

EventAction::EventAction(
    std::shared_ptr<G4GO::Optical::OpticalEventBridge> bridge,
    std::shared_ptr<EventOutputQueue> output) :
    fBridge{std::move(bridge)},
    fOutput{std::move(output)} {}

auto EventAction::BeginOfEventAction(const G4Event* event) -> void {
    fBridge->BeginEvent(event->GetEventID());
}

auto EventAction::EndOfEventAction(const G4Event* event) -> void {
    const auto transportFuture{fBridge->EndEvent()};

    auto scintillatorHCid{
        G4SDManager::GetSDMpointer()->GetCollectionID(
            "ScintillatorHitsCollection")};
    auto scintHC{static_cast<G4GO::Detector::ScintillatorHC*>(
        event->GetHCofThisEvent()->GetHC(scintillatorHCid))};

    auto sensorHCid{
        G4SDManager::GetSDMpointer()->GetCollectionID("SensorHitsCollection")};
    auto sensorHC{static_cast<G4GO::Detector::SensorHC*>(
        event->GetHCofThisEvent()->GetHC(sensorHCid))};

    const auto moduleID{static_cast<const G4GO::Detector::DetectorConstruction*>(
                            G4RunManager::GetRunManager()->GetUserDetectorConstruction())
                            ->ModuleID()};
    auto eventID{event->GetEventID()};

    if (fBridge->BackendType() == G4GO::Optical::Backend::Optix) {
        std::vector<CrystalHitRow> crystalHits{};
        crystalHits.reserve(moduleID);
        for (auto i{0}; i < moduleID; i++) {
            const auto energyDeposit{(*scintHC)[i]->GetEnergyDeposit()};
            if (energyDeposit > 0.) {
                crystalHits.push_back({eventID, i, energyDeposit});
            }
        }
        fOutput->Submit({eventID, std::move(crystalHits), transportFuture});
        fOutput->DrainReady(fOutput->Size() > 64);
        return;
    }

    for (const auto& photonHit : fBridge->EventHits()) {
        auto* sensorHit{new G4GO::Detector::SensorHit()};
        sensorHit->SetGlobalTime(photonHit.fTimeNs * ns);
        sensorHit->SetCopyNo(static_cast<G4int>(photonHit.fSensorID));
        sensorHC->insert(sensorHit);
    }

    auto analysisManager{G4AnalysisManager::Instance()};

    for (auto i{0}; i < moduleID; i++) {

        auto energyDeposit{(*scintHC)[i]->GetEnergyDeposit()};

        if (energyDeposit > 0.) {
            analysisManager->FillNtupleIColumn(0, 0, eventID);
            analysisManager->FillNtupleIColumn(0, 1, i);
            analysisManager->FillNtupleDColumn(0, 2, energyDeposit);
            analysisManager->FillNtupleIColumn(0, 3, sensorHC->entries());
            analysisManager->AddNtupleRow(0);
        }
    }

    for (long unsigned int j{}; j < sensorHC->entries(); j++) {

        auto sensorHit{(*sensorHC)[j]};

        analysisManager->FillNtupleIColumn(1, 0, eventID);
        analysisManager->FillNtupleIColumn(1, 1, sensorHit->GetCopyNo());
        analysisManager->FillNtupleDColumn(1, 2, sensorHit->GetGlobalTime());
        analysisManager->AddNtupleRow(1);
    }
}

} // namespace G4GO::Simulation
