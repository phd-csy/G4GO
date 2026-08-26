#include "g4go/optical/geant4/Geant4BatchScheduler.hpp"

#include "G4Exception.hh"
#include "G4TransportationManager.hh"
#include "G4ios.hh"
#include "Geant4SceneExporter.hpp"

#ifdef G4GO_ENABLE_OPTIX
#    include "OptiXDeviceData.cuh"
#    include "cuda_runtime_api.h"
#    include "g4go/optical/optix/OptiXTransportHost.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace G4GO::Optical {

Geant4BatchScheduler::Geant4BatchScheduler(
    PhotonTransportConfig configuration) :
    fConfiguration{std::move(configuration)},
    fBackend{fConfiguration.fBackend} {}

Geant4BatchScheduler::~Geant4BatchScheduler() {
    EndRun();
}

auto Geant4BatchScheduler::BeginRun() -> void {
    std::unique_lock lock{fMutex};
    if (fStarted) {
        fCondition.wait(lock, [this] { return fReady; });
        return;
    }
    fStarted = true;
    fBackend = fConfiguration.fBackend;

    if (fBackend == PhotonTransportBackend::Auto) {
#ifdef G4GO_ENABLE_OPTIX
        fBackend = PhotonTransportBackend::OptiX;
#else
        fBackend = PhotonTransportBackend::Geant4;
#endif
    }

    if (fBackend != PhotonTransportBackend::OptiX) {
        fReady = true;
        lock.unlock();
        fCondition.notify_all();
        return;
    }

    const auto* world{
        G4TransportationManager::GetTransportationManager()
            ->GetNavigatorForTracking()
            ->GetWorldVolume()};
    try {
        fScene = Geant4SceneExporter{fConfiguration.fMeshRotationSteps}
                     .Export(world);
    } catch (const std::exception& exception) {
        if (fConfiguration.fBackend == PhotonTransportBackend::Auto) {
            fBackend = PhotonTransportBackend::Geant4;
            G4cout << "[g4go] OptiX scene export failed: " << exception.what()
                   << "; falling back to Geant4 backend" << G4endl;
        } else {
            SetFatalError("Geant4BatchScheduler::BeginRun", exception);
        }
        fReady = true;
        lock.unlock();
        fCondition.notify_all();
        return;
    }

#ifdef G4GO_ENABLE_OPTIX
    fGpuThread = std::thread([this] { ProcessBatches(); });
    fCondition.wait(lock, [this] { return fReady; });
    if (fFailure != nullptr &&
        fConfiguration.fBackend == PhotonTransportBackend::Auto) {
        fBackend = PhotonTransportBackend::Geant4;
        fFailure = nullptr;
        fStopRequested = true;
        lock.unlock();
        fCondition.notify_all();
        if (fGpuThread.joinable()) {
            fGpuThread.join();
        }
        lock.lock();
        fStopRequested = false;
        fReady = true;
        lock.unlock();
        G4cout << "[g4go] OptiX initialization failed; falling back to "
                  "Geant4 backend"
               << G4endl;
        return;
    }
    if (fFailure != nullptr) {
        const auto error{fFailure};
        lock.unlock();
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exception) {
            SetFatalError("Geant4BatchScheduler::BeginRun", exception);
        }
    }
#else
    const auto exception{std::runtime_error{
        "OptiX backend requested, but this build does not contain the OptiX "
        "backend"}};
    SetFatalError("Geant4BatchScheduler::BeginRun", exception);
    fReady = true;
    lock.unlock();
    fCondition.notify_all();
#endif
}

auto Geant4BatchScheduler::EndRun() -> void {
    {
        std::lock_guard lock{fMutex};
        if (!fStarted || fStopRequested) {
            return;
        }
        fStopRequested = true;
    }
    fCondition.notify_all();
    if (fGpuThread.joinable()) {
        fGpuThread.join();
    }
#ifdef G4GO_ENABLE_OPTIX
    fPhotonTransport.reset();
#endif
    std::lock_guard lock{fMutex};
    fScene = {};
    fFailure = nullptr;
    fReady = false;
    fStarted = false;
    fStopRequested = false;
}

auto Geant4BatchScheduler::Schedule(
    std::int32_t eventID,
    std::vector<Photon> photons,
    PhotonTransportStatistics sourceStatistics) -> PhotonTransportFuture {
    auto promise{
        std::make_shared<std::promise<PhotonTransportOutput>>()};
    auto future{promise->get_future().share()};
    if (photons.empty() ||
        SelectedBackend() != PhotonTransportBackend::OptiX) {
        PhotonTransportOutput result{};
        result.fStatistics = sourceStatistics;
        promise->set_value(std::move(result));
        return future;
    }

    std::unique_lock lock{fMutex};
    const auto queueLimit{std::max<std::size_t>(
        1, fConfiguration.fMaxQueuedPhotons)};
    fCondition.wait(lock, [this, photonCount = photons.size(), queueLimit] {
        return fFailure != nullptr || fStopRequested ||
               fQueuedPhotonCount + photonCount <=
                   std::max<std::size_t>(
                       1, fConfiguration.fMaxQueuedPhotons) ||
               (fQueuedPhotonCount == 0 && photonCount > queueLimit);
    });
    if (fFailure != nullptr) {
        std::rethrow_exception(fFailure);
    }
    if (fStopRequested) {
        throw std::runtime_error("Photon batch scheduler is stopping");
    }

    fRunStatistics.fGeneratedCount += sourceStatistics.fGeneratedCount;
    fRunStatistics.fCapturedCount += sourceStatistics.fCapturedCount;
    fQueuedPhotonCount += photons.size();
    fBatchStatistics.fQueueHighWaterMark =
        std::max<std::uint64_t>(fBatchStatistics.fQueueHighWaterMark,
                                fQueuedPhotonCount);
    fPendingRequests.push_back(
        {eventID, std::move(photons), sourceStatistics, promise,
         std::chrono::steady_clock::now()});
    lock.unlock();
    fCondition.notify_all();
    return future;
}

auto Geant4BatchScheduler::SelectedBackend() const
    -> PhotonTransportBackend {
    std::lock_guard lock{fMutex};
    return fBackend;
}

auto Geant4BatchScheduler::RunStatistics() const
    -> const PhotonTransportStatistics& {
    return fRunStatistics;
}

auto Geant4BatchScheduler::BatchStatistics() const
    -> const PhotonBatchStatistics& {
    return fBatchStatistics;
}

auto Geant4BatchScheduler::ProcessBatches() -> void {
#ifdef G4GO_ENABLE_OPTIX
    try {
        std::size_t freeBytes{};
        std::size_t totalBytes{};
        if (cudaMemGetInfo(&freeBytes, &totalBytes) == cudaSuccess) {
            constexpr auto bytesPerPhoton{
                sizeof(DevicePhoton) + sizeof(DevicePhotonHit)};
            const auto safePhotonCount{
                std::max<std::size_t>(1, freeBytes / 3 / bytesPerPhoton)};
            if (fConfiguration.fTargetPhotonsPerBatch > safePhotonCount) {
                G4cout << "[g4go] reducing GPU batch target from "
                       << fConfiguration.fTargetPhotonsPerBatch << " to "
                       << safePhotonCount
                       << " photons for the available VRAM" << G4endl;
                fConfiguration.fTargetPhotonsPerBatch =
                    static_cast<std::uint32_t>(std::min<std::size_t>(
                        safePhotonCount,
                        std::numeric_limits<std::uint32_t>::max()));
            }
        }
        fPhotonTransport =
            std::make_unique<OptiXTransportHost>(fConfiguration);
        {
            std::lock_guard lock{fMutex};
            fReady = true;
        }
        fCondition.notify_all();

        for (;;) {
            std::vector<TransportRequest> requests{};
            std::size_t photonCount{};
            {
                std::unique_lock lock{fMutex};
                fCondition.wait(lock, [this] {
                    return fStopRequested || !fPendingRequests.empty();
                });
                if (fPendingRequests.empty() && fStopRequested) {
                    break;
                }

                const auto deadline{
                    fPendingRequests.front().fQueuedAt +
                    std::chrono::milliseconds(
                        fConfiguration.fBatchCollectionTimeoutMs)};
                while (photonCount <
                           fConfiguration.fTargetPhotonsPerBatch &&
                       requests.size() <
                           fConfiguration.fMaxEventsPerBatch) {
                    if (fPendingRequests.empty()) {
                        if (fStopRequested || photonCount == 0 ||
                            !fCondition.wait_until(
                                lock, deadline,
                                [this] {
                                    return fStopRequested ||
                                           !fPendingRequests.empty();
                                })) {
                            break;
                        }
                    }
                    if (fPendingRequests.empty()) {
                        break;
                    }
                    auto request{std::move(fPendingRequests.front())};
                    fPendingRequests.pop_front();
                    photonCount += request.fPhotons.size();
                    fQueuedPhotonCount -= request.fPhotons.size();
                    requests.push_back(std::move(request));
                    if (photonCount >=
                            fConfiguration.fTargetPhotonsPerBatch ||
                        requests.size() >=
                            fConfiguration.fMaxEventsPerBatch) {
                        break;
                    }
                }
                if (requests.empty() && !fPendingRequests.empty()) {
                    auto request{std::move(fPendingRequests.front())};
                    fPendingRequests.pop_front();
                    photonCount = request.fPhotons.size();
                    fQueuedPhotonCount -= photonCount;
                    requests.push_back(std::move(request));
                }
            }
            fCondition.notify_all();
            if (requests.empty()) {
                continue;
            }

            std::vector<Photon> batch{};
            batch.reserve(photonCount);
            std::unordered_map<std::uint32_t, std::size_t> eventToRequest{};
            for (auto index{std::size_t{}}; index < requests.size(); ++index) {
                eventToRequest.emplace(
                    static_cast<std::uint32_t>(requests[index].fEventID),
                    index);
                batch.insert(batch.end(), requests[index].fPhotons.begin(),
                             requests[index].fPhotons.end());
            }
            const auto start{std::chrono::steady_clock::now()};
            PhotonTransportOutput transportResult{};
            try {
                transportResult = fPhotonTransport->Propagate(fScene, batch);
            } catch (...) {
                const auto error{std::current_exception()};
                for (auto& request : requests) {
                    request.fPromise->set_exception(error);
                }
                std::rethrow_exception(error);
            }
            const auto elapsed{std::chrono::duration<double, std::milli>{
                std::chrono::steady_clock::now() - start}
                                   .count()};

            std::vector<PhotonTransportOutput> eventResults(requests.size());
            for (auto index{std::size_t{}}; index < requests.size(); ++index) {
                eventResults[index].fStatistics =
                    requests[index].fSourceStatistics;
                eventResults[index].fStatistics.fTransportTimeMs =
                    photonCount == 0 ?
                        0.0 :
                        elapsed * requests[index].fPhotons.size() /
                            static_cast<double>(photonCount);
            }
            for (const auto& detection : transportResult.fDetections) {
                const auto iterator{
                    eventToRequest.find(detection.fEventID)};
                if (iterator != eventToRequest.end()) {
                    eventResults[iterator->second].fDetections.push_back(
                        detection);
                    ++eventResults[iterator->second]
                          .fStatistics.fDetectedCount;
                }
            }
            for (auto index{std::size_t{}}; index < requests.size(); ++index) {
                auto& eventStatistics{eventResults[index].fStatistics};
                eventStatistics.fAbsorbedCount = 0;
                eventStatistics.fEscapedCount = 0;
                eventStatistics.fTruncatedCount = 0;
                eventStatistics.fInvalidStateCount = 0;
                eventStatistics.fZeroStepCount = 0;
                eventStatistics.fMaxBounceCount = 0;
                requests[index].fPromise->set_value(
                    std::move(eventResults[index]));
            }

            const auto& batchStatistics{transportResult.fStatistics};
            fRunStatistics.fDetectedCount += batchStatistics.fDetectedCount;
            fRunStatistics.fAbsorbedCount += batchStatistics.fAbsorbedCount;
            fRunStatistics.fEscapedCount += batchStatistics.fEscapedCount;
            fRunStatistics.fTruncatedCount += batchStatistics.fTruncatedCount;
            fRunStatistics.fMaxBounceCount =
                std::max(fRunStatistics.fMaxBounceCount,
                         batchStatistics.fMaxBounceCount);
            fRunStatistics.fInvalidStateCount +=
                batchStatistics.fInvalidStateCount;
            fRunStatistics.fZeroStepCount += batchStatistics.fZeroStepCount;
            fRunStatistics.fTransportTimeMs += elapsed;
            ++fBatchStatistics.fBatchCount;
            fBatchStatistics.fEventCount += requests.size();
            fBatchStatistics.fPhotonCount += photonCount;
            fBatchStatistics.fMaxBatchEventCount =
                std::max<std::uint64_t>(
                    fBatchStatistics.fMaxBatchEventCount, requests.size());
            fBatchStatistics.fMaxBatchPhotonCount =
                std::max<std::uint64_t>(
                    fBatchStatistics.fMaxBatchPhotonCount, photonCount);
        }
        fPhotonTransport.reset();
    } catch (...) {
        const auto error{std::current_exception()};
        {
            std::lock_guard lock{fMutex};
            fFailure = error;
            fReady = true;
            fStopRequested = true;
        }
        FailPendingRequests(error);
        fCondition.notify_all();
    }
#endif
}

auto Geant4BatchScheduler::FailPendingRequests(std::exception_ptr error)
    -> void {
    std::deque<TransportRequest> pending{};
    {
        std::lock_guard lock{fMutex};
        pending.swap(fPendingRequests);
        fQueuedPhotonCount = 0;
    }
    for (auto& request : pending) {
        request.fPromise->set_exception(error);
    }
}

auto Geant4BatchScheduler::SetFatalError(const char* operation,
                                         const std::exception& error) -> void {
    G4ExceptionDescription description{};
    description << error.what();
    G4Exception(operation, "G4GOOpticalBackend", FatalException, description);
}

} // namespace G4GO::Optical
