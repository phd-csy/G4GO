#include "g4go/simulation/RunAction.hpp"

#include "G4AnalysisManager.hh"
#include "G4ios.hh"
#include "g4go/optical/geant4/EventBridge.hpp"
#include "g4go/optical/geant4/OpticalBatchService.hpp"
#include "g4go/simulation/EventOutput.hpp"

#include <utility>

namespace G4GO::Simulation {

RunAction::RunAction(
    std::shared_ptr<G4GO::Optical::OpticalBatchService> batchService,
    std::shared_ptr<G4GO::Optical::OpticalEventBridge> bridge,
    std::shared_ptr<EventOutputQueue> output,
    bool isMaster) :
    G4UserRunAction{},
    fBridge{std::move(bridge)},
    fBatchService{std::move(batchService)},
    fOutput{std::move(output)},
    fIsMaster{isMaster} {
    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->SetDefaultFileType("root");

#ifdef G4MULTITHREADED
    analysisManager->SetNtupleMerging(true);
#endif

    analysisManager->CreateNtuple("CrystalHit", "crystal hit results");
    analysisManager->CreateNtupleIColumn("eventID");
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
    } else if (fBatchService) {
        fBatchService->BeginRun();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->OpenFile();
}

auto RunAction::EndOfRunAction(const G4Run*) -> void {
    if (fOutput) {
        fOutput->Flush();
    }
    if (fBridge) {
        fBridge->EndRun();
    }
    if (fIsMaster && fBatchService &&
        fBatchService->BackendType() != G4GO::Optical::Backend::Geant4) {
        fBatchService->EndRun();
        const auto& stats{fBatchService->RunStats()};
        const auto& batches{fBatchService->Statistics()};
        G4cout << "[g4go] optical backend: "
               << G4GO::Optical::BackendName(fBatchService->BackendType())
               << ", generated: " << stats.fGeneratedCount
               << ", captured: " << stats.fCapturedCount
               << ", detected: " << stats.fDetectedCount
               << ", absorbed: " << stats.fAbsorbedCount
               << ", escaped: " << stats.fEscapedCount
               << ", truncated: " << stats.fTruncatedCount
               << ", invalid: " << stats.fInvalidStateCount
               << ", max_bounce: " << stats.fMaxBounceCount
               << ", transport_ms: " << stats.fTransportTimeMs
               << ", batches: " << batches.fBatchCount
               << ", batch_photons: " << batches.fPhotonCount
               << ", max_batch_photons: " << batches.fMaxBatchPhotonCount
               << G4endl;
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->Write();
    analysisManager->CloseFile();
}

} // namespace G4GO::Simulation
