#include "g4go/optical/geant4/Geant4BatchScheduler.hpp"

#include "G4TransportationManager.hh"
#include "Geant4SceneExporter.hpp"

#ifdef G4GO_ENABLE_OPTIX
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
        if (fFailure != nullptr) {
            const auto error{fFailure};
            lock.unlock();
            std::rethrow_exception(error);
        }
        return;
    }

    fRunStatistics = {};
    fBatchStatistics = {};
    fPerformance = {};
    fBatchBuffer.clear();
    fPendingRequests.clear();
    fQueuedPhotonCount = 0;
    fQueuedSubmissionCount = 0;
    fFailure = nullptr;
    fStopRequested = false;
    fReady = false;
    fStarted = true;
    fBackend = fConfiguration.fBackend;

    if (fBackend == PhotonTransportBackend::Auto) {
#ifdef G4GO_ENABLE_OPTIX
        fBackend = PhotonTransportBackend::OptiX;
#else
        fBackend = PhotonTransportBackend::Geant4;
#endif
    }

    if (fBackend == PhotonTransportBackend::Geant4) {
        fReady = true;
        lock.unlock();
        fCondition.notify_all();
        return;
    }

#ifdef G4GO_ENABLE_OPTIX
    const auto* world{
        G4TransportationManager::GetTransportationManager()
            ->GetNavigatorForTracking()
            ->GetWorldVolume()};
    try {
        fScene = Geant4SceneExporter{fConfiguration.fMeshRotationSteps}
                     .Export(world);
    } catch (...) {
        fFailure = std::current_exception();
        fReady = true;
        const auto error{fFailure};
        lock.unlock();
        fCondition.notify_all();
        std::rethrow_exception(error);
    }

    fGpuThread = std::thread([this] { ProcessBatches(); });
    fCondition.wait(lock, [this] { return fReady; });
    if (fFailure != nullptr) {
        const auto error{fFailure};
        lock.unlock();
        std::rethrow_exception(error);
    }
    lock.unlock();
#else
    const auto error{std::make_exception_ptr(std::runtime_error{
        "OptiX backend requested, but this build does not contain the OptiX "
        "backend"})};
    fFailure = error;
    fReady = true;
    lock.unlock();
    fCondition.notify_all();
    std::rethrow_exception(error);
#endif
}

auto Geant4BatchScheduler::EndRun() -> void {
    {
        std::lock_guard lock{fMutex};
        if (!fStarted) {
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
    fPendingRequests.clear();
    fQueuedPhotonCount = 0;
    fQueuedSubmissionCount = 0;
    fFailure = nullptr;
    fReady = false;
    fStarted = false;
    fStopRequested = false;
}

auto Geant4BatchScheduler::Submit(OpticalSubmission submission)
    -> PhotonTransportFuture {
    auto promise{
        std::make_shared<std::promise<PhotonTransportOutput>>()};
    auto future{promise->get_future().share()};

    if (SelectedBackend() != PhotonTransportBackend::OptiX) {
        throw std::logic_error(
            "optical submissions require the OptiX backend");
    }

    std::uint64_t countedPhotons{};
    for (const auto& emission : submission.fEmissions) {
        countedPhotons += emission.fPhotonCount;
    }
    if (countedPhotons != submission.fPhotonCount) {
        throw std::invalid_argument(
            "optical submission photon count does not match its emissions");
    }
    if (submission.fPhotonCount == 0 && !submission.fEmissions.empty()) {
        throw std::invalid_argument(
            "empty optical submissions cannot contain emissions");
    }
    if (submission.fPhotonCount != 0 && submission.fEmissions.empty()) {
        throw std::invalid_argument(
            "non-empty optical submissions require emissions");
    }

    std::unique_lock lock{fMutex};
    const auto maxQueuedPhotons{std::max<std::size_t>(
        1, fConfiguration.fMaxQueuedPhotons)};
    const auto maxQueuedSubmissions{std::max<std::size_t>(
        1, fConfiguration.fMaxQueuedSubmissions)};
    const auto photonCount{static_cast<std::size_t>(submission.fPhotonCount)};
    if (!fStarted || !fReady) {
        throw std::runtime_error(
            "optical submission queue is not ready");
    }
    if (fFailure != nullptr) {
        std::rethrow_exception(fFailure);
    }
    if (fStopRequested) {
        throw std::runtime_error("Photon submission queue is stopping");
    }

    if (submission.fPhotonCount == 0) {
        fRunStatistics.fGeneratedCount +=
            submission.fSourceStatistics.fGeneratedCount;
        fRunStatistics.fCapturedCount +=
            submission.fSourceStatistics.fCapturedCount;
        fPerformance.Accumulate(submission.fSourcePerformance);
        fBatchStatistics.fPerformance = fPerformance;
        PhotonTransportOutput output{};
        output.fStatistics = submission.fSourceStatistics;
        output.fPerformance = submission.fSourcePerformance;
        lock.unlock();
        promise->set_value(std::move(output));
        return future;
    }

    fCondition.wait(lock, [this, photonCount, maxQueuedPhotons,
                           maxQueuedSubmissions] {
        return fFailure != nullptr || fStopRequested ||
               (fQueuedSubmissionCount < maxQueuedSubmissions &&
                fQueuedPhotonCount + photonCount <= maxQueuedPhotons) ||
               (fQueuedSubmissionCount == 0 && photonCount > maxQueuedPhotons);
    });
    if (fFailure != nullptr) {
        std::rethrow_exception(fFailure);
    }
    if (fStopRequested) {
        throw std::runtime_error("Photon submission queue is stopping");
    }

    fRunStatistics.fGeneratedCount +=
        submission.fSourceStatistics.fGeneratedCount;
    fRunStatistics.fCapturedCount +=
        submission.fSourceStatistics.fCapturedCount;
    fPerformance.Accumulate(submission.fSourcePerformance);
    fBatchStatistics.fPerformance = fPerformance;
    fQueuedPhotonCount += photonCount;
    ++fQueuedSubmissionCount;
    fBatchStatistics.fQueueHighWaterMark =
        std::max<std::uint64_t>(fBatchStatistics.fQueueHighWaterMark,
                                fQueuedPhotonCount);
    fPendingRequests.emplace_back(
        TransportRequest{std::move(submission), promise,
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

auto Geant4BatchScheduler::PerformanceStatistics() const
    -> const PhotonTransportPerformance& {
    return fPerformance;
}

auto Geant4BatchScheduler::CompleteBatch(
    std::vector<TransportRequest> requests,
    std::size_t photonCount,
    PhotonTransportOutput transportOutput) -> void {
    const auto elapsed{transportOutput.fStatistics.fTransportTimeMs};
    std::vector<PhotonTransportOutput> eventOutputs(requests.size());
    for (auto index{std::size_t{}}; index < requests.size(); ++index) {
        eventOutputs[index].fStatistics =
            requests.at(index).fSubmission.fSourceStatistics;
        eventOutputs[index].fPerformance =
            requests.at(index).fSubmission.fSourcePerformance;
        eventOutputs[index].fStatistics.fTransportTimeMs =
            photonCount == 0 ?
                0.0 :
                elapsed *
                    static_cast<double>(
                        requests.at(index).fSubmission.fPhotonCount) /
                    static_cast<double>(photonCount);
    }

    const auto invalidRequestIndex{std::numeric_limits<std::size_t>::max()};
    std::unordered_map<std::uint32_t, std::size_t> eventToRequest{};
    eventToRequest.reserve(requests.size());
    for (auto index{std::size_t{}}; index < requests.size(); ++index) {
        eventToRequest.emplace(requests.at(index).fSubmission.fEventID, index);
    }
    for (const auto& detection : transportOutput.fDetections) {
        const auto iterator{eventToRequest.find(detection.fEventID)};
        const auto requestIndex{iterator == eventToRequest.end() ?
                                    invalidRequestIndex :
                                    iterator->second};
        if (requestIndex != invalidRequestIndex) {
            eventOutputs[requestIndex].fDetections.emplace_back(detection);
            ++eventOutputs[requestIndex].fStatistics.fDetectedCount;
        }
    }
    for (auto index{std::size_t{}}; index < requests.size(); ++index) {
        auto& eventStatistics{eventOutputs[index].fStatistics};
        eventStatistics.fAbsorbedCount = 0;
        eventStatistics.fEscapedCount = 0;
        eventStatistics.fTruncatedCount = 0;
        eventStatistics.fInvalidStateCount = 0;
        eventStatistics.fZeroStepCount = 0;
        eventStatistics.fMaxBounceCount = 0;
        requests.at(index).fPromise->set_value(std::move(eventOutputs[index]));
    }

    const auto& batchStatistics{transportOutput.fStatistics};
    {
        std::lock_guard lock{fMutex};
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
        fPerformance.Accumulate(transportOutput.fPerformance);
        ++fBatchStatistics.fBatchCount;
        fBatchStatistics.fEventCount += requests.size();
        fBatchStatistics.fPhotonCount += photonCount;
        fBatchStatistics.fMaxBatchEventCount =
            std::max<std::uint64_t>(fBatchStatistics.fMaxBatchEventCount,
                                    requests.size());
        fBatchStatistics.fMaxBatchPhotonCount =
            std::max<std::uint64_t>(fBatchStatistics.fMaxBatchPhotonCount,
                                    photonCount);
        fBatchStatistics.fMinBatchPhotonCount =
            fBatchStatistics.fBatchCount == 1 ?
                photonCount :
                std::min<std::uint64_t>(fBatchStatistics.fMinBatchPhotonCount,
                                        photonCount);
        fBatchStatistics.fPerformance = fPerformance;
    }
}

auto Geant4BatchScheduler::ProcessBatches() -> void {
#ifdef G4GO_ENABLE_OPTIX
    std::vector<TransportRequest> requests{};
    try {
        fPhotonTransport = std::make_unique<OptiXTransportHost>(fConfiguration);
        fPhotonTransport->PrepareScene(fScene);
        {
            std::lock_guard lock{fMutex};
            fReady = true;
        }
        fCondition.notify_all();

        for (;;) {
            requests.clear();
            std::size_t photonCount{};
            {
                std::unique_lock lock{fMutex};
                if (fPendingRequests.empty() && !fStopRequested) {
                    fCondition.wait(lock, [this] {
                        return fStopRequested || !fPendingRequests.empty();
                    });
                }
                if (fPendingRequests.empty() && fStopRequested) {
                    break;
                }

                const auto deadline{
                    fPendingRequests.at(0).fQueuedAt +
                    std::chrono::milliseconds{
                        fConfiguration.fBatchCollectionTimeoutMs}};
                while (photonCount < fConfiguration.fTargetPhotonsPerBatch &&
                       requests.size() < fConfiguration.fMaxEventsPerBatch) {
                    if (fPendingRequests.empty()) {
                        if (fStopRequested || photonCount == 0 ||
                            !fCondition.wait_until(
                                lock, deadline, [this] {
                                    return fStopRequested ||
                                           !fPendingRequests.empty();
                                })) {
                            break;
                        }
                    }
                    if (fPendingRequests.empty()) {
                        break;
                    }
                    auto request{std::move(fPendingRequests.at(0))};
                    fPendingRequests.pop_front();
                    photonCount += static_cast<std::size_t>(
                        request.fSubmission.fPhotonCount);
                    fQueuedPhotonCount -= static_cast<std::size_t>(
                        request.fSubmission.fPhotonCount);
                    --fQueuedSubmissionCount;
                    if (fConfiguration.fEnablePerformanceDiagnostics) {
                        fPerformance.fSchedulerQueueWaitMs +=
                            std::chrono::duration<double, std::milli>{
                                std::chrono::steady_clock::now() -
                                request.fQueuedAt}
                                .count();
                        ++fBatchStatistics.fTotalQueueWaitCount;
                    }
                    requests.emplace_back(std::move(request));
                }
            }
            fCondition.notify_all();
            if (requests.empty()) {
                continue;
            }

            const auto flattenStart{
                fConfiguration.fEnablePerformanceDiagnostics ?
                    std::chrono::steady_clock::now() :
                    std::chrono::steady_clock::time_point{}};
            fBatchBuffer.clear();
            for (const auto& request : requests) {
                fBatchBuffer.insert(fBatchBuffer.end(),
                                    request.fSubmission.fEmissions.begin(),
                                    request.fSubmission.fEmissions.end());
            }
            if (fConfiguration.fEnablePerformanceDiagnostics) {
                const auto flattenMs{
                    std::chrono::duration<double, std::milli>{
                        std::chrono::steady_clock::now() - flattenStart}
                        .count()};
                std::lock_guard lock{fMutex};
                fPerformance.fSchedulerBatchFlattenMs += flattenMs;
                fPerformance.fSchedulerBatchFlattenBytes +=
                    static_cast<std::uint64_t>(fBatchBuffer.size() *
                                               sizeof(OpticalEmission));
                fBatchStatistics.fPerformance = fPerformance;
            }
            auto output{
                fPhotonTransport->PropagateEmissions(fScene, fBatchBuffer)};
            CompleteBatch(std::move(requests), photonCount, std::move(output));
        }
        fPhotonTransport.reset();
    } catch (...) {
        const auto error{std::current_exception()};
        for (auto& request : requests) {
            request.fPromise->set_exception(error);
        }
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
        fQueuedSubmissionCount = 0;
    }
    for (auto& request : pending) {
        request.fPromise->set_exception(error);
    }
}

} // namespace G4GO::Optical
