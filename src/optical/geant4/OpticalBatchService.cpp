#include "g4go/optical/geant4/OpticalBatchService.hpp"

#include "G4Exception.hh"
#include "G4TransportationManager.hh"
#include "G4VPhysicalVolume.hh"
#include "G4ios.hh"
#include "SceneExporter.hpp"

#ifdef G4GO_ENABLE_OPTIX
#    include "OptixTransportTypes.cuh"
#    include "cuda_runtime_api.h"
#    include "g4go/optical/optix/OptixTransport.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace G4GO::Optical {

OpticalBatchService::OpticalBatchService(TransportConfig config) :
    fConfig{std::move(config)},
    fBackend{fConfig.fBackend} {}

OpticalBatchService::~OpticalBatchService() {
    EndRun();
}

auto OpticalBatchService::BeginRun() -> void {
    std::unique_lock lock{fMutex};
    if (fStarted) {
        fCondition.wait(lock, [this] { return fReady; });
        return;
    }
    fStarted = true;
    fBackend = fConfig.fBackend;

    if (fBackend == Backend::Auto) {
#ifdef G4GO_ENABLE_OPTIX
        fBackend = Backend::Optix;
#else
        fBackend = Backend::Geant4;
#endif
    }

    if (fBackend != Backend::Optix) {
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
        fScene = Geant4SceneExporter{fConfig.fMeshRotationSteps}.Export(world);
    } catch (const std::exception& exception) {
        if (fConfig.fBackend == Backend::Auto) {
            fBackend = Backend::Geant4;
            G4cout << "[g4go] OptiX scene export failed: " << exception.what()
                   << "; falling back to Geant4 backend" << G4endl;
        } else {
            SetFatalError("OpticalBatchService::BeginRun", exception);
        }
        fReady = true;
        lock.unlock();
        fCondition.notify_all();
        return;
    }

#ifdef G4GO_ENABLE_OPTIX
    fGpuThread = std::thread([this] { RunGpuLoop(); });
    fCondition.wait(lock, [this] { return fReady; });
    if (fFailure != nullptr && fConfig.fBackend == Backend::Auto) {
        fBackend = Backend::Geant4;
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
            SetFatalError("OpticalBatchService::BeginRun", exception);
        }
    }
#else
    const auto exception{std::runtime_error{
        "OptiX backend requested, but this build does not contain the OptiX "
        "backend"}};
    SetFatalError("OpticalBatchService::BeginRun", exception);
    fReady = true;
    lock.unlock();
    fCondition.notify_all();
#endif
}

auto OpticalBatchService::EndRun() -> void {
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
    fTransport.reset();
#endif
    std::lock_guard lock{fMutex};
    fScene = {};
    fFailure = nullptr;
    fReady = false;
    fStarted = false;
    fStopRequested = false;
}

auto OpticalBatchService::Submit(std::int32_t eventID,
                                 std::vector<Photon> photonData,
                                 TransportStats sourceStats)
    -> EventTransportFuture {
    auto promise{std::make_shared<std::promise<TransportResult>>()};
    auto future{promise->get_future().share()};
    if (photonData.empty() || BackendType() != Backend::Optix) {
        TransportResult result{};
        result.fStats = sourceStats;
        promise->set_value(std::move(result));
        return future;
    }

    std::unique_lock lock{fMutex};
    const auto queueLimit{std::max<std::size_t>(
        1, fConfig.fBatchMaxQueuePhotons)};
    fCondition.wait(lock, [this, photonCount = photonData.size(), queueLimit] {
        return fFailure != nullptr || fStopRequested ||
               fQueuedPhotonCount + photonCount <=
                   std::max<std::size_t>(1, fConfig.fBatchMaxQueuePhotons) ||
               (fQueuedPhotonCount == 0 && photonCount > queueLimit);
    });
    if (fFailure != nullptr) {
        std::rethrow_exception(fFailure);
    }
    if (fStopRequested) {
        throw std::runtime_error("Optical batch service is stopping");
    }

    fRunStats.fGeneratedCount += sourceStats.fGeneratedCount;
    fRunStats.fCapturedCount += sourceStats.fCapturedCount;
    fQueuedPhotonCount += photonData.size();
    fBatchStats.fQueueHighWaterMark =
        std::max<std::uint64_t>(fBatchStats.fQueueHighWaterMark,
                                fQueuedPhotonCount);
    fQueue.push_back({eventID, std::move(photonData), sourceStats, promise,
                      std::chrono::steady_clock::now()});
    lock.unlock();
    fCondition.notify_all();
    return future;
}

auto OpticalBatchService::BackendType() const -> Backend {
    std::lock_guard lock{fMutex};
    return fBackend;
}

auto OpticalBatchService::RunStats() const -> const TransportStats& {
    return fRunStats;
}

auto OpticalBatchService::Statistics() const -> const BatchStats& {
    return fBatchStats;
}

auto OpticalBatchService::RunGpuLoop() -> void {
#ifdef G4GO_ENABLE_OPTIX
    try {
        std::size_t freeBytes{};
        std::size_t totalBytes{};
        if (cudaMemGetInfo(&freeBytes, &totalBytes) == cudaSuccess) {
            constexpr auto bytesPerPhoton{
                sizeof(DevicePhoton) + sizeof(DevicePhotonHit)};
            const auto safePhotonCount{
                std::max<std::size_t>(1, freeBytes / 3 / bytesPerPhoton)};
            if (fConfig.fBatchPhotonCount > safePhotonCount) {
                G4cout << "[g4go] reducing GPU batch target from "
                       << fConfig.fBatchPhotonCount << " to "
                       << safePhotonCount
                       << " photons for the available VRAM" << G4endl;
                fConfig.fBatchPhotonCount =
                    static_cast<std::uint32_t>(std::min<std::size_t>(
                        safePhotonCount,
                        std::numeric_limits<std::uint32_t>::max()));
            }
        }
        fTransport = std::make_unique<OptixOpticalTransport>(fConfig);
        {
            std::lock_guard lock{fMutex};
            fReady = true;
        }
        fCondition.notify_all();

        for (;;) {
            std::vector<Job> jobs{};
            std::size_t photonCount{};
            {
                std::unique_lock lock{fMutex};
                fCondition.wait(lock, [this] {
                    return fStopRequested || !fQueue.empty();
                });
                if (fQueue.empty() && fStopRequested) {
                    break;
                }

                const auto deadline{
                    fQueue.front().fQueuedAt +
                    std::chrono::milliseconds(fConfig.fBatchTimeoutMs)};
                while (photonCount < fConfig.fBatchPhotonCount &&
                       jobs.size() < fConfig.fBatchMaxEvents) {
                    if (fQueue.empty()) {
                        if (fStopRequested || photonCount == 0 ||
                            !fCondition.wait_until(
                                lock, deadline,
                                [this] {
                                    return fStopRequested || !fQueue.empty();
                                })) {
                            break;
                        }
                    }
                    if (fQueue.empty()) {
                        break;
                    }
                    auto job{std::move(fQueue.front())};
                    fQueue.pop_front();
                    photonCount += job.fPhotonData.size();
                    fQueuedPhotonCount -= job.fPhotonData.size();
                    jobs.push_back(std::move(job));
                    if (photonCount >= fConfig.fBatchPhotonCount ||
                        jobs.size() >= fConfig.fBatchMaxEvents) {
                        break;
                    }
                }
                if (jobs.empty() && !fQueue.empty()) {
                    auto job{std::move(fQueue.front())};
                    fQueue.pop_front();
                    photonCount = job.fPhotonData.size();
                    fQueuedPhotonCount -= photonCount;
                    jobs.push_back(std::move(job));
                }
            }
            fCondition.notify_all();
            if (jobs.empty()) {
                continue;
            }

            std::vector<Photon> batch{};
            batch.reserve(photonCount);
            std::unordered_map<std::uint32_t, std::size_t> eventToJob{};
            for (auto index{std::size_t{}}; index < jobs.size(); ++index) {
                eventToJob.emplace(static_cast<std::uint32_t>(jobs[index].fEventID),
                                   index);
                batch.insert(batch.end(), jobs[index].fPhotonData.begin(),
                             jobs[index].fPhotonData.end());
            }
            const auto start{std::chrono::steady_clock::now()};
            TransportResult transportResult{};
            try {
                transportResult = fTransport->Transport(fScene, batch);
            } catch (...) {
                const auto error{std::current_exception()};
                for (auto& job : jobs) {
                    job.fPromise->set_exception(error);
                }
                std::rethrow_exception(error);
            }
            const auto elapsed{std::chrono::duration<double, std::milli>{
                std::chrono::steady_clock::now() - start}
                                   .count()};

            std::vector<TransportResult> eventResults(jobs.size());
            for (auto index{std::size_t{}}; index < jobs.size(); ++index) {
                eventResults[index].fStats = jobs[index].fSourceStats;
                eventResults[index].fStats.fTransportTimeMs =
                    photonCount == 0 ? 0.0 : elapsed * jobs[index].fPhotonData.size() / static_cast<double>(photonCount);
            }
            for (const auto& hit : transportResult.fHitData) {
                const auto iterator{eventToJob.find(hit.fEventID)};
                if (iterator != eventToJob.end()) {
                    eventResults[iterator->second].fHitData.push_back(hit);
                    ++eventResults[iterator->second].fStats.fDetectedCount;
                }
            }
            for (auto index{std::size_t{}}; index < jobs.size(); ++index) {
                auto& eventStats{eventResults[index].fStats};
                eventStats.fAbsorbedCount = 0;
                eventStats.fEscapedCount = 0;
                eventStats.fTruncatedCount = 0;
                eventStats.fInvalidStateCount = 0;
                eventStats.fZeroStepCount = 0;
                eventStats.fMaxBounceCount = 0;
                jobs[index].fPromise->set_value(std::move(eventResults[index]));
            }

            fRunStats.fDetectedCount += transportResult.fStats.fDetectedCount;
            fRunStats.fAbsorbedCount += transportResult.fStats.fAbsorbedCount;
            fRunStats.fEscapedCount += transportResult.fStats.fEscapedCount;
            fRunStats.fTruncatedCount += transportResult.fStats.fTruncatedCount;
            fRunStats.fMaxBounceCount = std::max(
                fRunStats.fMaxBounceCount, transportResult.fStats.fMaxBounceCount);
            fRunStats.fInvalidStateCount +=
                transportResult.fStats.fInvalidStateCount;
            fRunStats.fZeroStepCount += transportResult.fStats.fZeroStepCount;
            fRunStats.fTransportTimeMs += elapsed;
            ++fBatchStats.fBatchCount;
            fBatchStats.fEventCount += jobs.size();
            fBatchStats.fPhotonCount += photonCount;
            fBatchStats.fMaxBatchEventCount =
                std::max<std::uint64_t>(fBatchStats.fMaxBatchEventCount,
                                        jobs.size());
            fBatchStats.fMaxBatchPhotonCount =
                std::max<std::uint64_t>(fBatchStats.fMaxBatchPhotonCount,
                                        photonCount);
        }
        fTransport.reset();
    } catch (...) {
        const auto error{std::current_exception()};
        {
            std::lock_guard lock{fMutex};
            fFailure = error;
            fReady = true;
            fStopRequested = true;
        }
        FailPendingJobs(error);
        fCondition.notify_all();
    }
#endif
}

auto OpticalBatchService::FailPendingJobs(std::exception_ptr error) -> void {
    std::deque<Job> pending{};
    {
        std::lock_guard lock{fMutex};
        pending.swap(fQueue);
        fQueuedPhotonCount = 0;
    }
    for (auto& job : pending) {
        job.fPromise->set_exception(error);
    }
}

auto OpticalBatchService::SetFatalError(const char* operation,
                                        const std::exception& error) -> void {
    G4ExceptionDescription description{};
    description << error.what();
    G4Exception(operation, "G4GOOpticalBackend", FatalException, description);
}

} // namespace G4GO::Optical
