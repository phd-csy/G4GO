#pragma once

#include "g4go/optical/PhotonTransport.hpp"
#include "g4go/optical/PhotonTransportConfig.hpp"
#include "g4go/optical/Scene.hpp"

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
    std::uint64_t batchCount{};
    std::uint64_t eventCount{};
    std::uint64_t photonCount{};
    std::uint64_t maxBatchPhotonCount{};
    std::uint64_t maxBatchEventCount{};
    std::uint64_t queueHighWaterMark{};
    std::uint64_t minBatchPhotonCount{};
    std::uint64_t totalQueueWaitCount{};
    double schedulerInputWaitMs{};
    double schedulerGpuWaitMs{};
    PhotonTransportPerformance performance{};
};

class OpticalBatchScheduler final {
public:
    explicit OpticalBatchScheduler(PhotonTransportConfig configuration);
    ~OpticalBatchScheduler();

    OpticalBatchScheduler(const OpticalBatchScheduler&) = delete;
    auto operator=(const OpticalBatchScheduler&) -> OpticalBatchScheduler& = delete;

    auto BeginRun() -> void;
    auto EndRun() -> void;

    auto Submit(OpticalSubmission submission) -> PhotonTransportFuture;

    auto SelectedBackend() const -> PhotonTransportBackend;
    auto PerformanceDiagnosticsEnabled() const -> bool { return fConfiguration.enablePerformanceDiagnostics; }
    auto ExportedScene() const -> const Scene& { return fScene; }
    auto RunStatistics() const -> const PhotonTransportStatistics&;
    auto BatchStatistics() const -> const PhotonBatchStatistics&;
    auto PerformanceStatistics() const -> const PhotonTransportPerformance&;

private:
    struct TransportRequest {
        OpticalSubmission submission{};
        std::shared_ptr<std::promise<PhotonTransportOutput>> promise{};
        std::chrono::steady_clock::time_point queuedAt{};
    };

    auto ProcessBatches() -> void;
    auto CompleteBatch(std::vector<TransportRequest>& requests, std::size_t photonCount,
                       PhotonTransportOutput transportOutput) -> void;
    auto FailPendingRequests(std::exception_ptr error) -> void;

    PhotonTransportConfig fConfiguration;
    PhotonTransportBackend fBackend;
    Scene fScene;
#ifdef G4GO_ENABLE_OPTIX
    std::unique_ptr<class OptiXTransportHost> fPhotonTransport;
#endif

    mutable std::mutex fMutex;
    std::condition_variable fCondition;
    std::deque<TransportRequest> fPendingRequests;
    std::size_t fQueuedPhotonCount;
    std::size_t fQueuedSubmissionCount;
    std::thread fGpuThread;
    std::exception_ptr fFailure;
    bool fStarted;
    bool fReady;
    bool fStopRequested;

    PhotonTransportStatistics fRunStatistics;
    PhotonBatchStatistics fBatchStatistics;
    PhotonTransportPerformance fPerformance;
    std::vector<OpticalEmission> fBatchBuffer;
};

} // namespace G4GO::Optical
