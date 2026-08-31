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
    std::uint64_t fBatchCount{};
    std::uint64_t fEventCount{};
    std::uint64_t fPhotonCount{};
    std::uint64_t fMaxBatchPhotonCount{};
    std::uint64_t fMaxBatchEventCount{};
    std::uint64_t fQueueHighWaterMark{};
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

    auto Schedule(std::int32_t eventID,
                  std::vector<Photon> photons,
                  PhotonTransportStatistics sourceStatistics)
        -> PhotonTransportFuture;

    auto SelectedBackend() const -> PhotonTransportBackend;
    auto ExportedScene() const -> const Scene& { return fScene; }
    auto RunStatistics() const -> const PhotonTransportStatistics&;
    auto BatchStatistics() const -> const PhotonBatchStatistics&;

private:
    struct TransportRequest {
        std::int32_t fEventID{};
        std::vector<Photon> fPhotons{};
        PhotonTransportStatistics fSourceStatistics{};
        std::shared_ptr<std::promise<PhotonTransportOutput>> fPromise{};
        std::chrono::steady_clock::time_point fQueuedAt{};
    };

    auto ProcessBatches() -> void;
    auto FailPendingRequests(std::exception_ptr error) -> void;
    auto SetFatalError(const char* operation, const std::exception& error)
        -> void;

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
    std::thread fGpuThread{};
    std::exception_ptr fFailure{};
    bool fStarted{};
    bool fReady{};
    bool fStopRequested{};

    PhotonTransportStatistics fRunStatistics{};
    PhotonBatchStatistics fBatchStatistics{};
};

} // namespace G4GO::Optical
