#include "g4go/simulation/RunAction.hpp"

#include "G4AnalysisManager.hh"
#include "G4ios.hh"
#include "g4go/optical/geant4/Geant4EventAdapter.hpp"
#include "g4go/optical/geant4/OpticalBatchScheduler.hpp"
#include "g4go/simulation/Analysis.hpp"

#include <filesystem>
#include <utility>

namespace G4GO::Simulation {

RunAction::RunAction(std::shared_ptr<G4GO::Optical::OpticalBatchScheduler> batchScheduler,
                     std::shared_ptr<G4GO::Optical::Geant4EventAdapter> adapter, std::shared_ptr<Analysis> analysis,
                     bool isMaster) :
    G4UserRunAction{},
    fAdapter{std::move(adapter)},
    fBatchScheduler{std::move(batchScheduler)},
    fAnalysis{std::move(analysis)},
    fIsMaster{isMaster} {
    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->SetDefaultFileType("root");

#ifdef G4MULTITHREADED
    analysisManager->SetNtupleMerging(false);
#endif

    analysisManager->CreateNtuple("CrystalHit", "crystal hit output");
    analysisManager->CreateNtupleIColumn("eventID");
    analysisManager->CreateNtupleIColumn("moduleID");
    analysisManager->CreateNtupleDColumn("Edep");
    analysisManager->CreateNtupleIColumn("nOptPho");
    analysisManager->CreateNtupleIColumn("nGenOptPho");
    analysisManager->FinishNtuple();

    analysisManager->CreateNtuple("SensorHit", "sensor hit output");
    analysisManager->CreateNtupleIColumn("eventID");
    auto& sensorIDs{fAnalysis ? fAnalysis->SensorIDs() : fDummySensorIDs};
    auto& sensorTimes{fAnalysis ? fAnalysis->SensorTimes() : fDummySensorTimes};
    analysisManager->CreateNtupleIColumn("sensorID", sensorIDs);
    analysisManager->CreateNtupleFColumn("timeOfFlight", sensorTimes);
    analysisManager->FinishNtuple();
}

auto RunAction::BeginOfRunAction(const G4Run*) -> void {
    if (fAdapter) {
        fAdapter->BeginRun();
    } else if (fBatchScheduler) {
        fBatchScheduler->BeginRun();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    const std::filesystem::path fileName{analysisManager->GetFileName().c_str()};
    if (fileName.empty()) {
        return;
    }
    const auto outputFileName{fileName.filename()};
    const auto& outputDirectory{outputFileName};
    if (not outputDirectory.empty()) {
        std::filesystem::create_directories(outputDirectory);
        analysisManager->SetFileName((outputDirectory / outputFileName).string());
    }
    analysisManager->OpenFile();
}

auto RunAction::EndOfRunAction(const G4Run*) -> void {
    if (fAnalysis) {
        fAnalysis->Flush();
    }
    if (fAdapter) {
        fAdapter->EndRun();
        if (fAdapter->Configuration().backend == G4GO::Optical::PhotonTransportBackend::Geant4 and
            fAdapter->Configuration().enablePerformanceDiagnostics) {
            G4cout << "[g4go] cpu generated=" << fAdapter->RunStatistics().generatedCount << G4endl;
        }
        if (fAdapter->Configuration().enablePerformanceDiagnostics) {
            const auto& performance{fAdapter->RunPerformance()};
            G4cout << "[g4go] source performance: cerenkov_photon_count=" << performance.cerenkovPhotonCount
                   << ", scintillation_photon_count=" << performance.scintillationPhotonCount << G4endl;
        }
    }
    if (fIsMaster and fBatchScheduler and
        fBatchScheduler->SelectedBackend() != G4GO::Optical::PhotonTransportBackend::Geant4) {
        fBatchScheduler->EndRun();
        const auto& statistics{fBatchScheduler->RunStatistics()};
        const auto& batches{fBatchScheduler->BatchStatistics()};
        G4cout << "[g4go] optical backend: " << G4GO::Optical::ToString(fBatchScheduler->SelectedBackend())
               << ", generated: " << statistics.generatedCount << ", captured: " << statistics.capturedCount
               << ", detected: " << statistics.detectedCount << ", absorbed: " << statistics.absorbedCount
               << ", escaped: " << statistics.escapedCount << ", truncated: " << statistics.truncatedCount
               << ", invalid: " << statistics.invalidStateCount << ", max bounce: " << statistics.maxBounceCount
               << ", transport time: " << statistics.transportTimeMs << " ms"
               << ", batches: " << batches.batchCount << ", batch photons: " << batches.photonCount
               << ", max batch photons: " << batches.maxBatchPhotonCount << G4endl;
        if (fBatchScheduler->PerformanceDiagnosticsEnabled()) {
            const auto& performance{fBatchScheduler->PerformanceStatistics()};
            const auto averageBatchPhotons{batches.batchCount == 0 ? 0.0 :
                                                                     static_cast<double>(batches.photonCount) /
                                                                         static_cast<double>(batches.batchCount)};
            G4cout << "[g4go] performance: capture total=" << performance.captureTotalMs
                   << ", capture locate volume=" << performance.captureLocateVolumeMs
                   << ", capture volume mapping=" << performance.captureVolumeMappingMs
                   << ", scheduler queue wait=" << performance.schedulerQueueWaitMs
                   << ", scheduler batch flatten=" << performance.schedulerBatchFlattenMs
                   << ", scheduler batch flatten bytes=" << performance.schedulerBatchFlattenBytes
                   << ", host to device=" << performance.hostToDeviceMs
                   << ", host to device bytes=" << performance.hostToDeviceBytes
                   << ", device memset=" << performance.deviceMemsetMs << ", optix kernel=" << performance.optiXKernelMs
                   << ", device hit compaction=" << performance.deviceHitCompactionMs
                   << ", device to host=" << performance.deviceToHostMs
                   << ", device metadata to host=" << performance.deviceMetadataToHostMs
                   << ", device metadata to host bytes=" << performance.deviceMetadataToHostBytes
                   << ", device hits to host=" << performance.deviceHitsToHostMs
                   << ", device hits to host bytes=" << performance.deviceHitsToHostBytes
                   << ", host hit compaction=" << performance.hostHitCompactionMs
                   << ", total bounce count=" << performance.totalBounceCount
                   << ", coincident candidate trace count=" << performance.coincidentCandidateTraceCount
                   << ", coincident candidate hit count=" << performance.coincidentCandidateHitCount
                   << ", cerenkov photon count=" << performance.cerenkovPhotonCount
                   << ", scintillation photon count=" << performance.scintillationPhotonCount
                   << ", detected count=" << statistics.detectedCount << ", absorbed count=" << statistics.absorbedCount
                   << ", escaped count=" << statistics.escapedCount << ", truncated count=" << statistics.truncatedCount
                   << ", invalid state count=" << statistics.invalidStateCount
                   << ", queue wait count=" << batches.totalQueueWaitCount
                   << ", scheduler input wait=" << batches.schedulerInputWaitMs
                   << ", scheduler gpu wait=" << batches.schedulerGpuWaitMs
                   << ", average batch photons=" << averageBatchPhotons
                   << ", min batch photons=" << batches.minBatchPhotonCount << G4endl;
        }
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    if (not analysisManager->IsOpenFile()) {
        return;
    }
    analysisManager->Write();
    analysisManager->CloseFile();
}

} // namespace G4GO::Simulation
