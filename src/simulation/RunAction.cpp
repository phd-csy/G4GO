#include "g4go/simulation/RunAction.hpp"

#include "G4AnalysisManager.hh"
#include "G4ios.hh"
#include "g4go/optical/geant4/Geant4EventAdapter.hpp"
#include "g4go/optical/geant4/Geant4BatchScheduler.hpp"
#include "g4go/simulation/Analysis.hpp"

#include <utility>

namespace G4GO::Simulation {

RunAction::RunAction(
    std::shared_ptr<G4GO::Optical::Geant4BatchScheduler> batchScheduler,
    std::shared_ptr<G4GO::Optical::Geant4EventAdapter> adapter,
    std::shared_ptr<Analysis> analysis,
    bool isMaster) :
    G4UserRunAction{},
    fAdapter{std::move(adapter)},
    fBatchScheduler{std::move(batchScheduler)},
    fAnalysis{std::move(analysis)},
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
    if (fAdapter) {
        fAdapter->BeginRun();
    } else if (fBatchScheduler) {
        fBatchScheduler->BeginRun();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->OpenFile();
}

auto RunAction::EndOfRunAction(const G4Run*) -> void {
    if (fAnalysis) {
        fAnalysis->Flush();
    }
    if (fAdapter) {
        fAdapter->EndRun();
    }
    if (fIsMaster && fBatchScheduler &&
        fBatchScheduler->SelectedBackend() !=
            G4GO::Optical::PhotonTransportBackend::Geant4) {
        fBatchScheduler->EndRun();
        const auto& statistics{fBatchScheduler->RunStatistics()};
        const auto& batches{fBatchScheduler->BatchStatistics()};
        G4cout << "[g4go] optical backend: "
               << G4GO::Optical::ToString(
                      fBatchScheduler->SelectedBackend())
               << ", generated: " << statistics.fGeneratedCount
               << ", captured: " << statistics.fCapturedCount
               << ", detected: " << statistics.fDetectedCount
               << ", absorbed: " << statistics.fAbsorbedCount
               << ", escaped: " << statistics.fEscapedCount
               << ", truncated: " << statistics.fTruncatedCount
               << ", invalid: " << statistics.fInvalidStateCount
               << ", max_bounce: " << statistics.fMaxBounceCount
               << ", transport_ms: " << statistics.fTransportTimeMs
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
