#include "g4go/simulation/RunAction.hpp"

#include "G4AnalysisManager.hh"
#include "G4ios.hh"
#include "g4go/optical/geant4/EventBridge.hpp"

#include <utility>

namespace G4GO::Simulation {

RunAction::RunAction(
    std::shared_ptr<G4GO::Optical::OpticalEventBridge> bridge) :
    G4UserRunAction{},
    fBridge{std::move(bridge)} {
    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->SetDefaultFileType("root");

#ifdef G4MULTITHREADED
    if (!fBridge ||
        fBridge->BackendType() == G4GO::Optical::Backend::Geant4) {
        analysisManager->SetNtupleMerging(true);
    }
#endif

    analysisManager->CreateNtuple("CrystalHit", "crystal hit results");
    analysisManager->CreateNtupleIColumn("moduleID");
    analysisManager->CreateNtupleDColumn("Edep");
    analysisManager->CreateNtupleIColumn("nOptPho");
    analysisManager->FinishNtuple();

    analysisManager->CreateNtuple("SensorHit", "sensor hit results");
    analysisManager->CreateNtupleIColumn("eventID");
    analysisManager->CreateNtupleIColumn("sensorID");
    analysisManager->CreateNtupleDColumn("timeOfFlight");
    analysisManager->FinishNtuple();
}

auto RunAction::BeginOfRunAction(const G4Run*) -> void {
    if (fBridge) {
        fBridge->BeginRun();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->OpenFile();
}

auto RunAction::EndOfRunAction(const G4Run*) -> void {
    if (fBridge) {
        fBridge->EndRun();
        if (fBridge->BackendType() != G4GO::Optical::Backend::Geant4) {
            const auto& stats{fBridge->RunStats()};
            G4cout << "[g4go] optical backend: "
                   << G4GO::Optical::BackendName(fBridge->BackendType())
                   << ", generated: " << stats.fGeneratedCount
                   << ", captured: " << stats.fCapturedCount
                   << ", detected: " << stats.fDetectedCount
                   << ", absorbed: " << stats.fAbsorbedCount
                   << ", escaped: " << stats.fEscapedCount
                   << ", invalid: " << stats.fInvalidStateCount
                   << ", max_bounce: " << stats.fMaxBounceCount
                   << ", transport_ms: " << stats.fTransportTimeMs << G4endl;
        }
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->Write();
    analysisManager->CloseFile();
}

} // namespace G4GO::Simulation
