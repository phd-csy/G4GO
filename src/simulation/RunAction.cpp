#include "g4go/simulation/RunAction.hpp"

#include "G4AnalysisManager.hh"
#include "G4ios.hh"
#include "g4go/optical/geant4/G4GOBatchScheduler.hpp"
#include "g4go/optical/geant4/G4GOEventAdapter.hpp"
#include "g4go/simulation/Analysis.hpp"

#include <chrono>
#include <utility>

namespace G4GO::Simulation {

RunAction::RunAction(
    std::shared_ptr<G4GO::Optical::G4GOBatchScheduler> batchScheduler,
    std::shared_ptr<G4GO::Optical::G4GOEventAdapter> adapter,
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

    analysisManager->CreateNtuple("CrystalHit", "crystal hit output");
    analysisManager->CreateNtupleIColumn("eventID");
    analysisManager->CreateNtupleIColumn("moduleID");
    analysisManager->CreateNtupleDColumn("Edep");
    analysisManager->CreateNtupleIColumn("nOptPho");
    analysisManager->CreateNtupleIColumn("nGenOptPho");
    analysisManager->FinishNtuple();

    analysisManager->CreateNtuple("SensorHit", "sensor hit output");
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
        if (fAdapter->Configuration().fBackend ==
                G4GO::Optical::PhotonTransportBackend::Geant4 &&
            fAdapter->Configuration().fEnablePerformanceDiagnostics) {
            G4cout << "[g4go] cpu generated="
                   << fAdapter->RunStatistics().fGeneratedCount << G4endl;
        }
        if (fAdapter->Configuration().fEnablePerformanceDiagnostics) {
            const auto& performance{fAdapter->RunPerformance()};
            G4cout << "[g4go] source performance: cerenkov_photon_count="
                   << performance.fCerenkovPhotonCount
                   << ", scintillation_photon_count="
                   << performance.fScintillationPhotonCount << G4endl;
        }
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
        if (fBatchScheduler->PerformanceDiagnosticsEnabled()) {
            const auto& performance{
                fBatchScheduler->PerformanceStatistics()};
            const auto averageBatchPhotons{
                batches.fBatchCount == 0 ?
                    0.0 :
                    static_cast<double>(batches.fPhotonCount) /
                        static_cast<double>(batches.fBatchCount)};
            G4cout << "[g4go] performance: capture_total_ms="
                   << performance.fCaptureTotalMs
                   << ", capture_locate_volume_ms="
                   << performance.fCaptureLocateVolumeMs
                   << ", capture_volume_mapping_ms="
                   << performance.fCaptureVolumeMappingMs
                   << ", scheduler_queue_wait_ms="
                   << performance.fSchedulerQueueWaitMs
                   << ", scheduler_batch_flatten_ms="
                   << performance.fSchedulerBatchFlattenMs
                   << ", scheduler_batch_flatten_bytes="
                   << performance.fSchedulerBatchFlattenBytes
                   << ", host_to_device_ms=" << performance.fHostToDeviceMs
                   << ", device_memset_ms=" << performance.fDeviceMemsetMs
                   << ", optix_kernel_ms=" << performance.fOptiXKernelMs
                   << ", device_hit_compaction_ms="
                   << performance.fDeviceHitCompactionMs
                   << ", device_to_host_ms=" << performance.fDeviceToHostMs
                   << ", host_hit_compaction_ms="
                   << performance.fHostHitCompactionMs
                   << ", total_bounce_count="
                   << performance.fTotalBounceCount
                   << ", coincident_candidate_trace_count="
                   << performance.fCoincidentCandidateTraceCount
                   << ", coincident_candidate_hit_count="
                   << performance.fCoincidentCandidateHitCount
                   << ", cerenkov_photon_count="
                   << performance.fCerenkovPhotonCount
                   << ", scintillation_photon_count="
                   << performance.fScintillationPhotonCount
                   << ", detected_count=" << statistics.fDetectedCount
                   << ", absorbed_count=" << statistics.fAbsorbedCount
                   << ", escaped_count=" << statistics.fEscapedCount
                   << ", truncated_count=" << statistics.fTruncatedCount
                   << ", invalid_state_count="
                   << statistics.fInvalidStateCount
                   << ", queue_wait_count=" << batches.fTotalQueueWaitCount
                   << ", max_in_flight_batches="
                   << batches.fMaxInFlightBatchCount
                   << ", scheduler_input_wait_ms="
                   << batches.fSchedulerInputWaitMs
                   << ", scheduler_gpu_wait_ms="
                   << batches.fSchedulerGpuWaitMs
                   << ", average_batch_photons=" << averageBatchPhotons
                   << ", min_batch_photons=" << batches.fMinBatchPhotonCount
                   << G4endl;
        }
    }

    const auto diagnostics{
        fBatchScheduler && fBatchScheduler->PerformanceDiagnosticsEnabled()};
    const auto rootOutputStart{
        diagnostics ? std::chrono::steady_clock::now() :
                      std::chrono::steady_clock::time_point{}};
    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->Write();
    analysisManager->CloseFile();
    if (diagnostics) {
        const auto rootOutputMs{std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() -
            rootOutputStart}
                                    .count()};
        G4cout << "[g4go] performance_output: root_output_ms="
               << rootOutputMs << G4endl;
    }
}

} // namespace G4GO::Simulation
