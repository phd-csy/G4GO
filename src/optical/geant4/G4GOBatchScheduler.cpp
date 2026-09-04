#include "g4go/optical/geant4/G4GOBatchScheduler.hpp"

#include "G4GOSceneExporter.hpp"
#include "G4TransportationManager.hh"

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

G4GOBatchScheduler::G4GOBatchScheduler(
    PhotonTransportConfig configuration) :
    fConfiguration{std::move(configuration)},
    fBackend{fConfiguration.fBackend},
    fScene{},
#ifdef G4GO_ENABLE_OPTIX
    fPhotonTransport{},
#endif
    fMutex{},
    fCondition{},
    fPendingRequests{},
    fQueuedPhotonCount{},
    fQueuedSubmissionCount{},
    fGpuThread{},
    fFailure{},
    fStarted{},
    fReady{},
    fStopRequested{},
    fRunStatistics{},
    fBatchStatistics{},
    fPerformance{},
    fBatchBuffer{} {
}

G4GOBatchScheduler::~G4GOBatchScheduler() {
    EndRun();
}

auto G4GOBatchScheduler::BeginRun() -> void {
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
        fScene = G4GOSceneExporter{fConfiguration.fMeshRotationSteps}
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

auto G4GOBatchScheduler::EndRun() -> void {
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

auto G4GOBatchScheduler::Submit(OpticalSubmission submission)
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
        fRunStatistics.fValidFields |= submission.fSourceStatistics.fValidFields;
        const auto terminalFields{
            StatisticFieldBit(PhotonTransportStatisticField::Detected) |
            StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
            StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
            StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
            StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
            StatisticFieldBit(PhotonTransportStatisticField::InvalidState) |
            StatisticFieldBit(PhotonTransportStatisticField::ZeroStep)};
        fRunStatistics.fValidFields |= terminalFields;
        fPerformance.Accumulate(submission.fSourcePerformance);
        fBatchStatistics.fPerformance = fPerformance;
        PhotonTransportOutput output{};
        output.fStatistics = submission.fSourceStatistics;
        output.fStatistics.fValidFields |= terminalFields;
        output.fPerformance = submission.fSourcePerformance;
        output.fEventStatistics.emplace_back(PhotonTransportEventStatistics{
            submission.fEventID, output.fStatistics});
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
    fRunStatistics.fValidFields |= submission.fSourceStatistics.fValidFields;
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

auto G4GOBatchScheduler::SelectedBackend() const
    -> PhotonTransportBackend {
    std::lock_guard lock{fMutex};
    return fBackend;
}

auto G4GOBatchScheduler::RunStatistics() const
    -> const PhotonTransportStatistics& {
    return fRunStatistics;
}

auto G4GOBatchScheduler::BatchStatistics() const
    -> const PhotonBatchStatistics& {
    return fBatchStatistics;
}

auto G4GOBatchScheduler::PerformanceStatistics() const
    -> const PhotonTransportPerformance& {
    return fPerformance;
}

auto G4GOBatchScheduler::CompleteBatch(
    std::vector<TransportRequest> requests,
    std::size_t photonCount,
    PhotonTransportOutput transportOutput) -> void {
    const auto terminalFields{
        StatisticFieldBit(PhotonTransportStatisticField::Detected) |
        StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
        StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
        StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
        StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
        StatisticFieldBit(PhotonTransportStatisticField::InvalidState) |
        StatisticFieldBit(PhotonTransportStatisticField::ZeroStep)};
    const auto elapsed{transportOutput.fStatistics.fTransportTimeMs};
    std::vector<PhotonTransportOutput> eventOutputs(requests.size());
    for (auto index{std::size_t{}}; index < requests.size(); ++index) {
        eventOutputs.at(index).fStatistics =
            requests.at(index).fSubmission.fSourceStatistics;
        eventOutputs.at(index).fPerformance =
            requests.at(index).fSubmission.fSourcePerformance;
    }

    std::unordered_map<std::uint32_t, std::size_t> eventToRequest{};
    eventToRequest.reserve(requests.size());
    for (auto index{std::size_t{}}; index < requests.size(); ++index) {
        if (!eventToRequest
                 .emplace(requests.at(index).fSubmission.fEventID, index)
                 .second) {
            throw std::invalid_argument(
                "optical batch contains duplicate event IDs");
        }
    }
    if (transportOutput.fEventStatistics.size() != requests.size()) {
        throw std::runtime_error(
            "OptiX output event statistics count does not match the batch");
    }
    std::vector<bool> eventStatisticsSeen(requests.size(), false);
    std::vector<std::size_t> detectionCounts(requests.size(), 0);
    std::uint64_t eventCapturedCount{};
    std::uint64_t eventDetectedCount{};
    std::uint64_t eventAbsorbedCount{};
    std::uint64_t eventEscapedCount{};
    std::uint64_t eventTruncatedCount{};
    std::uint64_t eventInvalidStateCount{};
    std::uint64_t eventZeroStepCount{};
    std::uint64_t eventMaxBounceCount{};
    auto accumulateEventCounter{
        [](std::uint64_t& total, std::uint64_t value, const char* field) {
            if (value > std::numeric_limits<std::uint64_t>::max() - total) {
                throw std::overflow_error(std::string{
                                              "OptiX event statistics overflow in "} +
                                          field);
            }
            total += value;
        }};
    for (const auto& eventOutput : transportOutput.fEventStatistics) {
        const auto iterator{eventToRequest.find(eventOutput.fEventID)};
        if (iterator == eventToRequest.end()) {
            throw std::runtime_error(
                "OptiX output contains an unknown event ID");
        }
        const auto requestIndex{iterator->second};
        if (eventStatisticsSeen.at(requestIndex)) {
            throw std::runtime_error(
                "OptiX output contains duplicate event statistics");
        }
        eventStatisticsSeen.at(requestIndex) = true;
        const auto& sourceStatistics{eventOutput.fStatistics};
        const auto sourceFields{sourceStatistics.fValidFields};
        if ((sourceFields &
             StatisticFieldBit(PhotonTransportStatisticField::TransportTime)) !=
            0U) {
            throw std::runtime_error(
                "OptiX event transport time is unavailable per event");
        }
        if ((sourceFields &
             StatisticFieldBit(PhotonTransportStatisticField::Captured)) !=
            0U) {
            accumulateEventCounter(eventCapturedCount,
                                   sourceStatistics.fCapturedCount,
                                   "captured count");
        }
        if ((sourceFields &
             StatisticFieldBit(PhotonTransportStatisticField::Detected)) != 0U) {
            accumulateEventCounter(eventDetectedCount,
                                   sourceStatistics.fDetectedCount,
                                   "detected count");
        }
        if ((sourceFields &
             StatisticFieldBit(PhotonTransportStatisticField::Absorbed)) != 0U) {
            accumulateEventCounter(eventAbsorbedCount,
                                   sourceStatistics.fAbsorbedCount,
                                   "absorbed count");
        }
        if ((sourceFields &
             StatisticFieldBit(PhotonTransportStatisticField::Escaped)) != 0U) {
            accumulateEventCounter(eventEscapedCount,
                                   sourceStatistics.fEscapedCount,
                                   "escaped count");
        }
        if ((sourceFields &
             StatisticFieldBit(PhotonTransportStatisticField::Truncated)) !=
            0U) {
            accumulateEventCounter(eventTruncatedCount,
                                   sourceStatistics.fTruncatedCount,
                                   "truncated count");
        }
        if ((sourceFields & StatisticFieldBit(
                                PhotonTransportStatisticField::InvalidState)) != 0U) {
            accumulateEventCounter(eventInvalidStateCount,
                                   sourceStatistics.fInvalidStateCount,
                                   "invalid-state count");
        }
        if ((sourceFields & StatisticFieldBit(
                                PhotonTransportStatisticField::ZeroStep)) != 0U) {
            accumulateEventCounter(eventZeroStepCount,
                                   sourceStatistics.fZeroStepCount,
                                   "zero-step count");
        }
        if ((sourceFields &
             StatisticFieldBit(PhotonTransportStatisticField::MaxBounce)) !=
            0U) {
            eventMaxBounceCount =
                std::max(eventMaxBounceCount, sourceStatistics.fMaxBounceCount);
        }
        auto& destination{eventOutputs.at(requestIndex).fStatistics};
        const auto destinationSourceFields{destination.fValidFields};
        const auto outputFields{eventOutput.fStatistics.fValidFields};
        const auto assignIfValid{
            [&](PhotonTransportStatisticField field,
                std::uint64_t& destinationValue, std::uint64_t sourceValue) {
                if ((outputFields & StatisticFieldBit(field)) != 0U) {
                    destinationValue = sourceValue;
                }
            }};
        assignIfValid(PhotonTransportStatisticField::Detected,
                      destination.fDetectedCount,
                      eventOutput.fStatistics.fDetectedCount);
        assignIfValid(PhotonTransportStatisticField::Absorbed,
                      destination.fAbsorbedCount,
                      eventOutput.fStatistics.fAbsorbedCount);
        assignIfValid(PhotonTransportStatisticField::Escaped,
                      destination.fEscapedCount,
                      eventOutput.fStatistics.fEscapedCount);
        assignIfValid(PhotonTransportStatisticField::Truncated,
                      destination.fTruncatedCount,
                      eventOutput.fStatistics.fTruncatedCount);
        assignIfValid(PhotonTransportStatisticField::MaxBounce,
                      destination.fMaxBounceCount,
                      eventOutput.fStatistics.fMaxBounceCount);
        assignIfValid(PhotonTransportStatisticField::InvalidState,
                      destination.fInvalidStateCount,
                      eventOutput.fStatistics.fInvalidStateCount);
        assignIfValid(PhotonTransportStatisticField::ZeroStep,
                      destination.fZeroStepCount,
                      eventOutput.fStatistics.fZeroStepCount);
        destination.fValidFields =
            (destinationSourceFields & ~terminalFields) |
            (outputFields & terminalFields);
        destination.fValidFields &=
            ~StatisticFieldBit(PhotonTransportStatisticField::TransportTime);
        eventOutputs.at(requestIndex).fEventStatistics.emplace_back(PhotonTransportEventStatistics{eventOutput.fEventID, destination});
    }
    const auto& batchStatistics{transportOutput.fStatistics};
    const auto requireEventField{
        [&](PhotonTransportStatisticField field, bool eventFieldPresent,
            const char* name) {
            const auto bit{StatisticFieldBit(field)};
            if ((batchStatistics.fValidFields & bit) != 0U &&
                !eventFieldPresent) {
                throw std::runtime_error(std::string{
                                             "OptiX batch statistics field is missing from event "} +
                                         name);
            }
        }};
    const auto allEventFieldsPresent{
        [&](PhotonTransportStatisticField field) {
            return std::all_of(
                transportOutput.fEventStatistics.begin(),
                transportOutput.fEventStatistics.end(), [&](const auto& item) {
                    return (item.fStatistics.fValidFields &
                            StatisticFieldBit(field)) != 0U;
                });
        }};
    requireEventField(PhotonTransportStatisticField::Captured,
                      allEventFieldsPresent(
                          PhotonTransportStatisticField::Captured),
                      "captured count");
    requireEventField(PhotonTransportStatisticField::Detected,
                      allEventFieldsPresent(
                          PhotonTransportStatisticField::Detected),
                      "detected count");
    requireEventField(PhotonTransportStatisticField::Absorbed,
                      allEventFieldsPresent(
                          PhotonTransportStatisticField::Absorbed),
                      "absorbed count");
    requireEventField(PhotonTransportStatisticField::Escaped,
                      allEventFieldsPresent(
                          PhotonTransportStatisticField::Escaped),
                      "escaped count");
    requireEventField(PhotonTransportStatisticField::Truncated,
                      allEventFieldsPresent(
                          PhotonTransportStatisticField::Truncated),
                      "truncated count");
    requireEventField(PhotonTransportStatisticField::InvalidState,
                      allEventFieldsPresent(
                          PhotonTransportStatisticField::InvalidState),
                      "invalid-state count");
    requireEventField(PhotonTransportStatisticField::ZeroStep,
                      allEventFieldsPresent(
                          PhotonTransportStatisticField::ZeroStep),
                      "zero-step count");
    requireEventField(PhotonTransportStatisticField::MaxBounce,
                      allEventFieldsPresent(
                          PhotonTransportStatisticField::MaxBounce),
                      "maximum-bounce count");
    const auto requireCounterSum{
        [&](PhotonTransportStatisticField field, std::uint64_t eventValue,
            std::uint64_t batchValue, const char* name) {
            if ((batchStatistics.fValidFields & StatisticFieldBit(field)) !=
                    0U &&
                eventValue != batchValue) {
                throw std::runtime_error(std::string{
                                             "OptiX batch/event statistics mismatch in "} +
                                         name);
            }
        }};
    requireCounterSum(PhotonTransportStatisticField::Captured,
                      eventCapturedCount, batchStatistics.fCapturedCount,
                      "captured count");
    requireCounterSum(PhotonTransportStatisticField::Detected,
                      eventDetectedCount, batchStatistics.fDetectedCount,
                      "detected count");
    requireCounterSum(PhotonTransportStatisticField::Absorbed,
                      eventAbsorbedCount, batchStatistics.fAbsorbedCount,
                      "absorbed count");
    requireCounterSum(PhotonTransportStatisticField::Escaped,
                      eventEscapedCount, batchStatistics.fEscapedCount,
                      "escaped count");
    requireCounterSum(PhotonTransportStatisticField::Truncated,
                      eventTruncatedCount, batchStatistics.fTruncatedCount,
                      "truncated count");
    requireCounterSum(PhotonTransportStatisticField::InvalidState,
                      eventInvalidStateCount,
                      batchStatistics.fInvalidStateCount,
                      "invalid-state count");
    requireCounterSum(PhotonTransportStatisticField::ZeroStep,
                      eventZeroStepCount, batchStatistics.fZeroStepCount,
                      "zero-step count");
    if ((batchStatistics.fValidFields &
         StatisticFieldBit(PhotonTransportStatisticField::MaxBounce)) != 0U &&
        eventMaxBounceCount != batchStatistics.fMaxBounceCount) {
        throw std::runtime_error(
            "OptiX batch/event statistics mismatch in maximum-bounce count");
    }
    for (const auto& detection : transportOutput.fDetections) {
        const auto iterator{eventToRequest.find(detection.fEventID)};
        if (iterator == eventToRequest.end()) {
            throw std::runtime_error(
                "OptiX output contains a detection for an unknown event ID");
        }
        const auto requestIndex{iterator->second};
        eventOutputs.at(requestIndex).fDetections.emplace_back(detection);
        ++detectionCounts.at(requestIndex);
    }
    for (auto index{std::size_t{}}; index < requests.size(); ++index) {
        if (!eventStatisticsSeen.at(index)) {
            throw std::runtime_error(
                "OptiX output is missing event statistics");
        }
        const auto& eventStatistics{eventOutputs.at(index).fStatistics};
        if ((eventStatistics.fValidFields &
             StatisticFieldBit(PhotonTransportStatisticField::Detected)) !=
                0U &&
            eventStatistics.fDetectedCount != detectionCounts.at(index)) {
            throw std::runtime_error(
                "OptiX event detected count does not match compacted hits");
        }
        requests.at(index).fPromise->set_value(std::move(eventOutputs.at(index)));
    }

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
        fRunStatistics.fValidFields |= batchStatistics.fValidFields;
        if ((batchStatistics.fValidFields &
             StatisticFieldBit(PhotonTransportStatisticField::TransportTime)) !=
            0U) {
            fRunStatistics.fTransportTimeMs += elapsed;
        }
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

auto G4GOBatchScheduler::ProcessBatches() -> void {
#ifdef G4GO_ENABLE_OPTIX
    struct InFlightBatch {
        std::vector<TransportRequest> fRequests{};
        std::size_t fPhotonCount{};
    };

    std::vector<TransportRequest> requests{};
    std::deque<InFlightBatch> inFlightBatches{};
    try {
        fPhotonTransport = std::make_unique<OptiXTransportHost>(fConfiguration);
        fPhotonTransport->PrepareScene(fScene);
        {
            std::lock_guard lock{fMutex};
            fReady = true;
        }
        fCondition.notify_all();

        for (;;) {
            const auto maximumInFlight{static_cast<std::size_t>(
                fConfiguration.fMaxInFlightBatches)};
            bool stopRequested{};
            bool hasPendingRequests{};
            {
                std::lock_guard lock{fMutex};
                stopRequested = fStopRequested;
                hasPendingRequests = !fPendingRequests.empty();
            }
            if (!inFlightBatches.empty() &&
                (inFlightBatches.size() >= maximumInFlight ||
                 !hasPendingRequests || stopRequested)) {
                const auto waitStart{
                    fConfiguration.fEnablePerformanceDiagnostics ?
                        std::chrono::steady_clock::now() :
                        std::chrono::steady_clock::time_point{}};
                auto output{fPhotonTransport->CompleteOldestBatch()};
                if (fConfiguration.fEnablePerformanceDiagnostics) {
                    std::lock_guard lock{fMutex};
                    fBatchStatistics.fSchedulerGpuWaitMs +=
                        std::chrono::duration<double, std::milli>{
                            std::chrono::steady_clock::now() - waitStart}
                            .count();
                }
                auto completed{std::move(inFlightBatches.front())};
                inFlightBatches.pop_front();
                CompleteBatch(std::move(completed.fRequests),
                              completed.fPhotonCount, std::move(output));
                continue;
            }

            requests.clear();
            std::size_t photonCount{};
            {
                std::unique_lock lock{fMutex};
                if (fPendingRequests.empty() && !fStopRequested) {
                    const auto waitStart{
                        fConfiguration.fEnablePerformanceDiagnostics ?
                            std::chrono::steady_clock::now() :
                            std::chrono::steady_clock::time_point{}};
                    fCondition.wait(lock, [this] {
                        return fStopRequested || !fPendingRequests.empty();
                    });
                    if (fConfiguration.fEnablePerformanceDiagnostics) {
                        fBatchStatistics.fSchedulerInputWaitMs +=
                            std::chrono::duration<double, std::milli>{
                                std::chrono::steady_clock::now() - waitStart}
                                .count();
                    }
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
                            [&] {
                                const auto waitStart{
                                    fConfiguration
                                            .fEnablePerformanceDiagnostics ?
                                        std::chrono::steady_clock::now() :
                                        std::chrono::steady_clock::time_point{}};
                                const auto ready{fCondition.wait_until(
                                    lock, deadline, [this] {
                                        return fStopRequested ||
                                               !fPendingRequests.empty();
                                    })};
                                if (fConfiguration
                                        .fEnablePerformanceDiagnostics) {
                                    fBatchStatistics.fSchedulerInputWaitMs +=
                                        std::chrono::duration<double,
                                                              std::milli>{
                                            std::chrono::steady_clock::now() -
                                            waitStart}
                                            .count();
                                }
                                return !ready;
                            }()) {
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
            std::vector<std::uint32_t> batchEventIDs{};
            batchEventIDs.reserve(requests.size());
            std::vector<std::uint32_t> emissionEventIndices{};
            std::unordered_map<std::uint32_t, std::uint32_t> eventIndices{};
            eventIndices.reserve(requests.size());
            for (const auto& request : requests) {
                const auto eventID{request.fSubmission.fEventID};
                const auto eventIndex{static_cast<std::uint32_t>(
                    batchEventIDs.size())};
                if (!eventIndices.emplace(eventID, eventIndex).second) {
                    throw std::invalid_argument(
                        "optical batch contains duplicate event IDs");
                }
                batchEventIDs.emplace_back(eventID);
                for (const auto& emission : request.fSubmission.fEmissions) {
                    if (emission.fEventID != eventID) {
                        throw std::invalid_argument(
                            "optical emission event ID does not match its "
                            "submission");
                    }
                    fBatchBuffer.emplace_back(emission);
                    emissionEventIndices.emplace_back(eventIndex);
                }
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
            const PhotonTransportBatch batch{
                fBatchBuffer, batchEventIDs, emissionEventIndices};
            fPhotonTransport->EnqueueEmissions(fScene, batch);
            inFlightBatches.emplace_back(
                InFlightBatch{std::move(requests), photonCount});
            if (fConfiguration.fEnablePerformanceDiagnostics) {
                std::lock_guard lock{fMutex};
                fBatchStatistics.fMaxInFlightBatchCount =
                    std::max<std::uint64_t>(
                        fBatchStatistics.fMaxInFlightBatchCount,
                        inFlightBatches.size());
            }
        }
        fPhotonTransport.reset();
    } catch (...) {
        const auto error{std::current_exception()};
        for (auto& request : requests) {
            try {
                request.fPromise->set_exception(error);
            } catch (const std::future_error&) {
            }
        }
        for (auto& batch : inFlightBatches) {
            for (auto& request : batch.fRequests) {
                try {
                    request.fPromise->set_exception(error);
                } catch (const std::future_error&) {
                }
            }
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

auto G4GOBatchScheduler::FailPendingRequests(std::exception_ptr error)
    -> void {
    std::deque<TransportRequest> pending{};
    {
        std::lock_guard lock{fMutex};
        pending.swap(fPendingRequests);
        fQueuedPhotonCount = 0;
        fQueuedSubmissionCount = 0;
    }
    for (auto& request : pending) {
        try {
            request.fPromise->set_exception(error);
        } catch (const std::future_error&) {
        }
    }
}

} // namespace G4GO::Optical
