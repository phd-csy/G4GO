#include "g4go/simulation/RunAction.hpp"

#include "G4AnalysisManager.hh"
#include "G4ios.hh"
#include "g4go/optical/geant4/Geant4EventAdapter.hpp"
#include "g4go/optical/geant4/OpticalBatchScheduler.hpp"
#include "g4go/simulation/Analysis.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <utility>

namespace G4GO::Simulation {

namespace {

struct AnalysisPerformanceTotals {
    std::atomic<std::uint64_t> sensorPackNs{};
    std::atomic<std::uint64_t> sensorNtupleNs{};
    std::atomic<std::uint64_t> futureWaitNs{};
};

auto PublishedAnalysisPerformance() -> AnalysisPerformanceTotals& {
    static AnalysisPerformanceTotals totals{};
    return totals;
}

auto MillisecondsToNanoseconds(double milliseconds) -> std::uint64_t {
    return static_cast<std::uint64_t>(milliseconds * 1'000'000.0);
}

auto NanosecondsToMilliseconds(std::uint64_t nanoseconds) -> double {
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}

auto ResetPublishedAnalysisPerformance() -> void {
    auto& totals{PublishedAnalysisPerformance()};
    totals.sensorPackNs.store(0, std::memory_order_relaxed);
    totals.sensorNtupleNs.store(0, std::memory_order_relaxed);
    totals.futureWaitNs.store(0, std::memory_order_relaxed);
}

auto PublishAnalysisPerformance(const AnalysisPerformance& performance) -> void {
    auto& totals{PublishedAnalysisPerformance()};
    totals.sensorPackNs.fetch_add(MillisecondsToNanoseconds(performance.sensorPackMs), std::memory_order_relaxed);
    totals.sensorNtupleNs.fetch_add(MillisecondsToNanoseconds(performance.sensorNtupleMs),
                                     std::memory_order_relaxed);
    totals.futureWaitNs.fetch_add(MillisecondsToNanoseconds(performance.futureWaitMs), std::memory_order_relaxed);
}

auto ReadPublishedAnalysisPerformance() -> AnalysisPerformance {
    const auto& totals{PublishedAnalysisPerformance()};
    return AnalysisPerformance{
        NanosecondsToMilliseconds(totals.sensorPackNs.load(std::memory_order_relaxed)),
        NanosecondsToMilliseconds(totals.sensorNtupleNs.load(std::memory_order_relaxed)),
        NanosecondsToMilliseconds(totals.futureWaitNs.load(std::memory_order_relaxed))};
}

} // namespace

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
#ifdef G4MULTITHREADED
    if (fIsMaster) {
        ResetPublishedAnalysisPerformance();
    }
#else
    ResetPublishedAnalysisPerformance();
#endif
    if (fAnalysis) {
        fAnalysis->ResetPerformance();
    }

    if (fAdapter) {
        fAdapter->BeginRun();
    } else if (fBatchScheduler) {
        fBatchScheduler->BeginRun();
    }

    auto analysisManager{G4AnalysisManager::Instance()};
    const std::filesystem::path fileName{analysisManager->GetFileName().c_str()};
    const auto outputDirectory{fileName.filename()};
    if (!outputDirectory.empty()) {
        std::filesystem::create_directories(outputDirectory);
        analysisManager->SetFileName((outputDirectory / outputDirectory).string());
    }
    analysisManager->OpenFile();
}

auto RunAction::EndOfRunAction(const G4Run*) -> void {
    if (fAnalysis) {
        fAnalysis->Flush();
        PublishAnalysisPerformance(fAnalysis->Performance());
    }
    if (fAdapter) {
        fAdapter->EndRun();
        if (fAdapter->Configuration().backend == G4GO::Optical::PhotonTransportBackend::Geant4 &&
            fAdapter->Configuration().enablePerformanceDiagnostics) {
            G4cout << "[g4go] cpu generated=" << fAdapter->RunStatistics().generatedCount << G4endl;
        }
        if (fAdapter->Configuration().enablePerformanceDiagnostics) {
            const auto& performance{fAdapter->RunPerformance()};
            G4cout << "[g4go] source performance: cerenkov_photon_count=" << performance.cerenkovPhotonCount
                   << ", scintillation_photon_count=" << performance.scintillationPhotonCount << G4endl;
        }
    }
    if (fIsMaster && fBatchScheduler &&
        fBatchScheduler->SelectedBackend() != G4GO::Optical::PhotonTransportBackend::Geant4) {
        fBatchScheduler->EndRun();
        const auto& statistics{fBatchScheduler->RunStatistics()};
        const auto& batches{fBatchScheduler->BatchStatistics()};
        G4cout << "[g4go] optical backend: " << G4GO::Optical::ToString(fBatchScheduler->SelectedBackend())
               << ", generated: " << statistics.generatedCount << ", captured: " << statistics.capturedCount
               << ", detected: " << statistics.detectedCount << ", absorbed: " << statistics.absorbedCount
               << ", escaped: " << statistics.escapedCount << ", truncated: " << statistics.truncatedCount
               << ", invalid: " << statistics.invalidStateCount << ", max_bounce: " << statistics.maxBounceCount
               << ", transport_ms: " << statistics.transportTimeMs << ", batches: " << batches.batchCount
               << ", batch_photons: " << batches.photonCount << ", max_batch_photons: " << batches.maxBatchPhotonCount
               << G4endl;
        if (fBatchScheduler->PerformanceDiagnosticsEnabled()) {
            const auto& performance{fBatchScheduler->PerformanceStatistics()};
            const auto averageBatchPhotons{batches.batchCount == 0 ? 0.0 :
                                                                     static_cast<double>(batches.photonCount) /
                                                                         static_cast<double>(batches.batchCount)};
            G4cout << "[g4go] performance: capture_total_ms=" << performance.captureTotalMs
                   << ", capture_locate_volume_ms=" << performance.captureLocateVolumeMs
                   << ", capture_volume_mapping_ms=" << performance.captureVolumeMappingMs
                   << ", scheduler_queue_wait_ms=" << performance.schedulerQueueWaitMs
                   << ", scheduler_batch_flatten_ms=" << performance.schedulerBatchFlattenMs
                   << ", scheduler_batch_flatten_bytes=" << performance.schedulerBatchFlattenBytes
                   << ", host_to_device_ms=" << performance.hostToDeviceMs
                   << ", host_to_device_bytes=" << performance.hostToDeviceBytes
                   << ", device_memset_ms=" << performance.deviceMemsetMs
                   << ", optix_kernel_ms=" << performance.optiXKernelMs
                   << ", device_hit_compaction_ms=" << performance.deviceHitCompactionMs
                   << ", device_to_host_ms=" << performance.deviceToHostMs
                   << ", device_metadata_to_host_ms=" << performance.deviceMetadataToHostMs
                   << ", device_metadata_to_host_bytes=" << performance.deviceMetadataToHostBytes
                   << ", device_hits_to_host_ms=" << performance.deviceHitsToHostMs
                   << ", device_hits_to_host_bytes=" << performance.deviceHitsToHostBytes
                   << ", host_hit_compaction_ms=" << performance.hostHitCompactionMs
                   << ", total_bounce_count=" << performance.totalBounceCount
                   << ", coincident_candidate_trace_count=" << performance.coincidentCandidateTraceCount
                   << ", coincident_candidate_hit_count=" << performance.coincidentCandidateHitCount
                   << ", cerenkov_photon_count=" << performance.cerenkovPhotonCount
                   << ", scintillation_photon_count=" << performance.scintillationPhotonCount
                   << ", detected_count=" << statistics.detectedCount << ", absorbed_count=" << statistics.absorbedCount
                   << ", escaped_count=" << statistics.escapedCount << ", truncated_count=" << statistics.truncatedCount
                   << ", invalid_state_count=" << statistics.invalidStateCount
                   << ", queue_wait_count=" << batches.totalQueueWaitCount
                   << ", scheduler_input_wait_ms=" << batches.schedulerInputWaitMs
                   << ", scheduler_gpu_wait_ms=" << batches.schedulerGpuWaitMs
                   << ", average_batch_photons=" << averageBatchPhotons
                   << ", min_batch_photons=" << batches.minBatchPhotonCount << G4endl;
        }
    }

#ifdef G4MULTITHREADED
    const auto reportRootOutput{fIsMaster};
#else
    const auto reportRootOutput{true};
#endif
    const auto rootOutputStart{std::chrono::steady_clock::now()};
    auto analysisManager{G4AnalysisManager::Instance()};
    analysisManager->Write();
    analysisManager->CloseFile();
    const auto rootOutputMs{
        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - rootOutputStart}.count()};
    if (reportRootOutput) {
        const auto analysisPerformance{ReadPublishedAnalysisPerformance()};
        G4cout << "[g4go] performance_output: root_output_ms=" << rootOutputMs << G4endl;
        G4cout << "[g4go] analysis_performance: sensor_pack_ms=" << analysisPerformance.sensorPackMs
               << ", sensor_ntuple_ms=" << analysisPerformance.sensorNtupleMs
               << ", analysis_future_wait_ms=" << analysisPerformance.futureWaitMs << G4endl;
    }
}

} // namespace G4GO::Simulation
