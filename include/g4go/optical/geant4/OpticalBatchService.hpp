#pragma once

#include "g4go/optical/Scene.hpp"
#include "g4go/optical/Types.hpp"

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

struct BatchStats {
    std::uint64_t fBatchCount{};
    std::uint64_t fEventCount{};
    std::uint64_t fPhotonCount{};
    std::uint64_t fMaxBatchPhotonCount{};
    std::uint64_t fMaxBatchEventCount{};
    std::uint64_t fQueueHighWaterMark{};
};

class OpticalBatchService final {
public:
    explicit OpticalBatchService(TransportConfig config);
    ~OpticalBatchService();

    OpticalBatchService(const OpticalBatchService&) = delete;
    auto operator=(const OpticalBatchService&) -> OpticalBatchService& = delete;

    auto BeginRun() -> void;
    auto EndRun() -> void;

    auto Submit(std::int32_t eventID,
                std::vector<Photon> photonData,
                TransportStats sourceStats) -> EventTransportFuture;

    auto BackendType() const -> Backend;
    auto RunStats() const -> const TransportStats&;
    auto Statistics() const -> const BatchStats&;

private:
    struct Job {
        std::int32_t fEventID{};
        std::vector<Photon> fPhotonData{};
        TransportStats fSourceStats{};
        std::shared_ptr<std::promise<TransportResult>> fPromise{};
        std::chrono::steady_clock::time_point fQueuedAt{};
    };

    auto RunGpuLoop() -> void;
    auto FailPendingJobs(std::exception_ptr error) -> void;
    auto SetFatalError(const char* operation, const std::exception& error)
        -> void;

    TransportConfig fConfig{};
    Backend fBackend{Backend::Auto};
    Scene fScene{};
#ifdef G4GO_ENABLE_OPTIX
    std::unique_ptr<class OptixOpticalTransport> fTransport{};
#endif

    mutable std::mutex fMutex{};
    std::condition_variable fCondition{};
    std::deque<Job> fQueue{};
    std::size_t fQueuedPhotonCount{};
    std::thread fGpuThread{};
    std::exception_ptr fFailure{};
    bool fStarted{};
    bool fReady{};
    bool fStopRequested{};

    TransportStats fRunStats{};
    BatchStats fBatchStats{};
};

} // namespace G4GO::Optical
