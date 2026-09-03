#pragma once

#include "g4go/optical/PhotonTransport.hpp"
#include "g4go/optical/PhotonTransportConfig.hpp"
#include "g4go/optical/Scene.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace G4GO::Optical {

struct PhotonBatchStatistics {
    std::uint64_t fBatchCount{};
    std::uint64_t fEventCount{};
    std::uint64_t fPhotonCount{};
    std::uint64_t fMaxBatchPhotonCount{};
    std::uint64_t fMaxBatchEventCount{};
    std::uint64_t fQueueHighWaterMark{};
    std::uint64_t fMinBatchPhotonCount{};
    std::uint64_t fTotalQueueWaitCount{};
    PhotonTransportPerformance fPerformance{};
};

class Geant4BatchScheduler final {
public:
    explicit Geant4BatchScheduler(
        PhotonTransportConfig configuration);
    ~Geant4BatchScheduler();

    Geant4BatchScheduler(const Geant4BatchScheduler&) = delete;
    auto operator=(const Geant4BatchScheduler&) -> Geant4BatchScheduler& =
                                                       delete;

    auto BeginRun() -> void;
    auto EndRun() -> void;

    auto Submit(OpticalSubmission submission)
        -> PhotonTransportFuture;

    auto SelectedBackend() const -> PhotonTransportBackend;
    auto PerformanceDiagnosticsEnabled() const -> bool {
        return fConfiguration.fEnablePerformanceDiagnostics;
    }
    auto ExportedScene() const -> const Scene& { return fScene; }
    auto RunStatistics() const -> const PhotonTransportStatistics&;
    auto BatchStatistics() const -> const PhotonBatchStatistics&;
    auto PerformanceStatistics() const -> const PhotonTransportPerformance&;

private:
    struct TransportRequest {
        OpticalSubmission fSubmission{};
        std::shared_ptr<std::promise<PhotonTransportOutput>> fPromise{};
        std::chrono::steady_clock::time_point fQueuedAt{};
    };

    auto ProcessBatches() -> void;
    auto CompleteBatch(std::vector<TransportRequest> requests,
                       std::size_t photonCount,
                       PhotonTransportOutput transportOutput)
        -> void;
    auto FailPendingRequests(std::exception_ptr error) -> void;

    PhotonTransportConfig fConfiguration{};
    PhotonTransportBackend fBackend{PhotonTransportBackend::Auto};
    Scene fScene{};
#ifdef G4GO_ENABLE_OPTIX
    std::unique_ptr<class OptiXTransportHost> fPhotonTransport{};
#endif

    mutable std::mutex fMutex{};
    std::condition_variable fCondition{};
    std::deque<TransportRequest> fPendingRequests{};
    std::size_t fQueuedPhotonCount{};
    std::size_t fQueuedSubmissionCount{};
    std::thread fGpuThread{};
    std::exception_ptr fFailure{};
    bool fStarted{};
    bool fReady{};
    bool fStopRequested{};

    PhotonTransportStatistics fRunStatistics{};
    PhotonBatchStatistics fBatchStatistics{};
    PhotonTransportPerformance fPerformance{};
    std::vector<OpticalEmission> fBatchBuffer{};
};

} // namespace G4GO::Optical
