#include "g4go/optical/geant4/OpticalBatchScheduler.hpp"

#include "G4TransportationManager.hh"
#include "Geant4SceneExporter.hpp"

#ifdef G4GO_ENABLE_OPTIX
#    include "g4go/optical/optix/OptiXTransportHost.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace G4GO::Optical {

OpticalBatchScheduler::OpticalBatchScheduler(PhotonTransportConfig configuration) :
    fConfiguration{std::move(configuration)},
    fBackend{fConfiguration.backend},
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

OpticalBatchScheduler::~OpticalBatchScheduler() { EndRun(); }

auto OpticalBatchScheduler::BeginRun() -> void {
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
    fBackend = fConfiguration.backend;

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
    const auto* world{G4TransportationManager::GetTransportationManager()->GetNavigatorForTracking()->GetWorldVolume()};
    try {
        fScene = Geant4SceneExporter{fConfiguration.meshRotationSteps}.Export(world);
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
    const auto error{
        std::make_exception_ptr(std::runtime_error{"OptiX backend requested, but this build does not contain the OptiX "
                                                   "backend"})};
    fFailure = error;
    fReady = true;
    lock.unlock();
    fCondition.notify_all();
    std::rethrow_exception(error);
#endif
}

auto OpticalBatchScheduler::EndRun() -> void {
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

auto OpticalBatchScheduler::Submit(OpticalSubmission submission) -> PhotonTransportFuture {
    auto promise{std::make_shared<std::promise<PhotonTransportOutput>>()};
    auto future{promise->get_future().share()};

    if (SelectedBackend() != PhotonTransportBackend::OptiX) {
        throw std::logic_error("optical submissions require the OptiX backend");
    }

    std::uint64_t countedPhotons{};
    for (const auto& emission : submission.emissions) {
        countedPhotons += emission.photonCount;
    }
    if (countedPhotons != submission.photonCount) {
        throw std::invalid_argument("optical submission photon count does not match its emissions");
    }
    if (submission.photonCount == 0 && !submission.emissions.empty()) {
        throw std::invalid_argument("empty optical submissions cannot contain emissions");
    }
    if (submission.photonCount != 0 && submission.emissions.empty()) {
        throw std::invalid_argument("non-empty optical submissions require emissions");
    }

    std::unique_lock lock{fMutex};
    const auto maxQueuedPhotons{std::max<std::size_t>(1, fConfiguration.maxQueuedPhotons)};
    const auto maxQueuedSubmissions{std::max<std::size_t>(1, fConfiguration.maxQueuedSubmissions)};
    const auto photonCount{static_cast<std::size_t>(submission.photonCount)};
    if (!fStarted || !fReady) {
        throw std::runtime_error("optical submission queue is not ready");
    }
    if (fFailure != nullptr) {
        std::rethrow_exception(fFailure);
    }
    if (fStopRequested) {
        throw std::runtime_error("Photon submission queue is stopping");
    }

    if (submission.photonCount == 0) {
        fRunStatistics.generatedCount += submission.sourceStatistics.generatedCount;
        fRunStatistics.capturedCount += submission.sourceStatistics.capturedCount;
        fRunStatistics.validFields |= submission.sourceStatistics.validFields;
        const auto terminalFields{StatisticFieldBit(PhotonTransportStatisticField::Detected) |
                                  StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
                                  StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
                                  StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
                                  StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
                                  StatisticFieldBit(PhotonTransportStatisticField::InvalidState) |
                                  StatisticFieldBit(PhotonTransportStatisticField::ZeroStep)};
        fRunStatistics.validFields |= terminalFields;
        fPerformance.Accumulate(submission.sourcePerformance);
        fBatchStatistics.performance = fPerformance;
        PhotonTransportOutput output{};
        output.statistics = submission.sourceStatistics;
        output.statistics.validFields |= terminalFields;
        output.performance = submission.sourcePerformance;
        output.eventStatistics.emplace_back(PhotonTransportEventStatistics{submission.eventID, output.statistics});
        lock.unlock();
        promise->set_value(std::move(output));
        return future;
    }

    fCondition.wait(lock, [this, photonCount, maxQueuedPhotons, maxQueuedSubmissions] {
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

    fRunStatistics.generatedCount += submission.sourceStatistics.generatedCount;
    fRunStatistics.capturedCount += submission.sourceStatistics.capturedCount;
    fRunStatistics.validFields |= submission.sourceStatistics.validFields;
    fPerformance.Accumulate(submission.sourcePerformance);
    fBatchStatistics.performance = fPerformance;
    fQueuedPhotonCount += photonCount;
    ++fQueuedSubmissionCount;
    fBatchStatistics.queueHighWaterMark =
        std::max<std::uint64_t>(fBatchStatistics.queueHighWaterMark, fQueuedPhotonCount);
    fPendingRequests.emplace_back(TransportRequest{std::move(submission), promise, std::chrono::steady_clock::now()});
    lock.unlock();
    fCondition.notify_all();
    return future;
}

auto OpticalBatchScheduler::SelectedBackend() const -> PhotonTransportBackend {
    std::lock_guard lock{fMutex};
    return fBackend;
}

auto OpticalBatchScheduler::RunStatistics() const -> const PhotonTransportStatistics& { return fRunStatistics; }

auto OpticalBatchScheduler::BatchStatistics() const -> const PhotonBatchStatistics& { return fBatchStatistics; }

auto OpticalBatchScheduler::PerformanceStatistics() const -> const PhotonTransportPerformance& { return fPerformance; }

auto OpticalBatchScheduler::CompleteBatch(std::vector<TransportRequest>& requests, std::size_t photonCount,
                                          PhotonTransportOutput transportOutput) -> void {
    const auto terminalFields{StatisticFieldBit(PhotonTransportStatisticField::Detected) |
                              StatisticFieldBit(PhotonTransportStatisticField::Absorbed) |
                              StatisticFieldBit(PhotonTransportStatisticField::Escaped) |
                              StatisticFieldBit(PhotonTransportStatisticField::Truncated) |
                              StatisticFieldBit(PhotonTransportStatisticField::MaxBounce) |
                              StatisticFieldBit(PhotonTransportStatisticField::InvalidState) |
                              StatisticFieldBit(PhotonTransportStatisticField::ZeroStep)};
    const auto elapsed{transportOutput.statistics.transportTimeMs};
    std::vector<PhotonTransportOutput> eventOutputs(requests.size());
    for (std::size_t i {}; i < requests.size(); i++) {
        eventOutputs.at(i).statistics = requests.at(i).submission.sourceStatistics;
        eventOutputs.at(i).performance = requests.at(i).submission.sourcePerformance;
    }

    std::unordered_map<std::uint32_t, std::size_t> eventToRequest{};
    eventToRequest.reserve(requests.size());
    for (std::size_t i {}; i < requests.size(); i++) {
        eventToRequest.emplace(requests.at(i).submission.eventID, i);
    }
    if (transportOutput.eventStatistics.size() != requests.size()) {
        throw std::runtime_error("OptiX output event statistics count does not match the batch");
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
    auto commonEventFields{std::numeric_limits<std::uint32_t>::max()};
    const auto accumulateEventCounter{[](std::uint64_t& total, std::uint64_t value) { total += value; }};
    for (const auto& eventOutput : transportOutput.eventStatistics) {
        const auto iterator{eventToRequest.find(eventOutput.eventID)};
        if (iterator == eventToRequest.end()) {
            throw std::runtime_error("OptiX output contains an unknown event ID");
        }
        const auto requestIndex{iterator->second};
        if (eventStatisticsSeen.at(requestIndex)) {
            throw std::runtime_error("OptiX output contains duplicate event statistics");
        }
        eventStatisticsSeen.at(requestIndex) = true;
        const auto& sourceStatistics{eventOutput.statistics};
        const auto sourceFields{sourceStatistics.validFields};
        commonEventFields &= sourceFields;
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::TransportTime)) != 0U) {
            throw std::runtime_error("OptiX event transport time is unavailable per event");
        }
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::Captured)) != 0U) {
            accumulateEventCounter(eventCapturedCount, sourceStatistics.capturedCount);
        }
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::Detected)) != 0U) {
            accumulateEventCounter(eventDetectedCount, sourceStatistics.detectedCount);
        }
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::Absorbed)) != 0U) {
            accumulateEventCounter(eventAbsorbedCount, sourceStatistics.absorbedCount);
        }
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::Escaped)) != 0U) {
            accumulateEventCounter(eventEscapedCount, sourceStatistics.escapedCount);
        }
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::Truncated)) != 0U) {
            accumulateEventCounter(eventTruncatedCount, sourceStatistics.truncatedCount);
        }
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::InvalidState)) != 0U) {
            accumulateEventCounter(eventInvalidStateCount, sourceStatistics.invalidStateCount);
        }
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::ZeroStep)) != 0U) {
            accumulateEventCounter(eventZeroStepCount, sourceStatistics.zeroStepCount);
        }
        if ((sourceFields & StatisticFieldBit(PhotonTransportStatisticField::MaxBounce)) != 0U) {
            eventMaxBounceCount = std::max(eventMaxBounceCount, sourceStatistics.maxBounceCount);
        }
        auto& destination{eventOutputs.at(requestIndex).statistics};
        const auto destinationSourceFields{destination.validFields};
        const auto outputFields{eventOutput.statistics.validFields};
        const auto assignIfValid{
            [&](PhotonTransportStatisticField field, std::uint64_t& destinationValue, std::uint64_t sourceValue) {
                if ((outputFields & StatisticFieldBit(field)) != 0U) {
                    destinationValue = sourceValue;
                }
            }};
        assignIfValid(PhotonTransportStatisticField::Detected, destination.detectedCount,
                      eventOutput.statistics.detectedCount);
        assignIfValid(PhotonTransportStatisticField::Absorbed, destination.absorbedCount,
                      eventOutput.statistics.absorbedCount);
        assignIfValid(PhotonTransportStatisticField::Escaped, destination.escapedCount,
                      eventOutput.statistics.escapedCount);
        assignIfValid(PhotonTransportStatisticField::Truncated, destination.truncatedCount,
                      eventOutput.statistics.truncatedCount);
        assignIfValid(PhotonTransportStatisticField::MaxBounce, destination.maxBounceCount,
                      eventOutput.statistics.maxBounceCount);
        assignIfValid(PhotonTransportStatisticField::InvalidState, destination.invalidStateCount,
                      eventOutput.statistics.invalidStateCount);
        assignIfValid(PhotonTransportStatisticField::ZeroStep, destination.zeroStepCount,
                      eventOutput.statistics.zeroStepCount);
        destination.validFields = (destinationSourceFields & ~terminalFields) | (outputFields & terminalFields);
        destination.validFields &= ~StatisticFieldBit(PhotonTransportStatisticField::TransportTime);
        eventOutputs.at(requestIndex)
            .eventStatistics.emplace_back(PhotonTransportEventStatistics{eventOutput.eventID, destination});
    }
    const auto& batchStatistics{transportOutput.statistics};
    const auto requireEventField{[&](PhotonTransportStatisticField field, bool eventFieldPresent, const char* name) {
        const auto bit{StatisticFieldBit(field)};
        if ((batchStatistics.validFields & bit) != 0U && !eventFieldPresent) {
            throw std::runtime_error(std::string{"OptiX batch statistics field is missing from event "} + name);
        }
    }};
    const auto hasCommonEventField{
        [&](PhotonTransportStatisticField field) { return (commonEventFields & StatisticFieldBit(field)) != 0U; }};
    requireEventField(PhotonTransportStatisticField::Captured,
                      hasCommonEventField(PhotonTransportStatisticField::Captured), "captured count");
    requireEventField(PhotonTransportStatisticField::Detected,
                      hasCommonEventField(PhotonTransportStatisticField::Detected), "detected count");
    requireEventField(PhotonTransportStatisticField::Absorbed,
                      hasCommonEventField(PhotonTransportStatisticField::Absorbed), "absorbed count");
    requireEventField(PhotonTransportStatisticField::Escaped,
                      hasCommonEventField(PhotonTransportStatisticField::Escaped), "escaped count");
    requireEventField(PhotonTransportStatisticField::Truncated,
                      hasCommonEventField(PhotonTransportStatisticField::Truncated), "truncated count");
    requireEventField(PhotonTransportStatisticField::InvalidState,
                      hasCommonEventField(PhotonTransportStatisticField::InvalidState), "invalid-state count");
    requireEventField(PhotonTransportStatisticField::ZeroStep,
                      hasCommonEventField(PhotonTransportStatisticField::ZeroStep), "zero-step count");
    requireEventField(PhotonTransportStatisticField::MaxBounce,
                      hasCommonEventField(PhotonTransportStatisticField::MaxBounce), "maximum-bounce count");
    const auto requireCounterSum{
        [&](PhotonTransportStatisticField field, std::uint64_t eventValue, std::uint64_t batchValue, const char* name) {
            if ((batchStatistics.validFields & StatisticFieldBit(field)) != 0U && eventValue != batchValue) {
                throw std::runtime_error(std::string{"OptiX batch/event statistics mismatch in "} + name);
            }
        }};
    requireCounterSum(PhotonTransportStatisticField::Captured, eventCapturedCount, batchStatistics.capturedCount,
                      "captured count");
    requireCounterSum(PhotonTransportStatisticField::Detected, eventDetectedCount, batchStatistics.detectedCount,
                      "detected count");
    requireCounterSum(PhotonTransportStatisticField::Absorbed, eventAbsorbedCount, batchStatistics.absorbedCount,
                      "absorbed count");
    requireCounterSum(PhotonTransportStatisticField::Escaped, eventEscapedCount, batchStatistics.escapedCount,
                      "escaped count");
    requireCounterSum(PhotonTransportStatisticField::Truncated, eventTruncatedCount, batchStatistics.truncatedCount,
                      "truncated count");
    requireCounterSum(PhotonTransportStatisticField::InvalidState, eventInvalidStateCount,
                      batchStatistics.invalidStateCount, "invalid-state count");
    requireCounterSum(PhotonTransportStatisticField::ZeroStep, eventZeroStepCount, batchStatistics.zeroStepCount,
                      "zero-step count");
    if ((batchStatistics.validFields & StatisticFieldBit(PhotonTransportStatisticField::MaxBounce)) != 0U &&
        eventMaxBounceCount != batchStatistics.maxBounceCount) {
        throw std::runtime_error("OptiX batch/event statistics mismatch in maximum-bounce count");
    }
    for (const auto& detection : transportOutput.detections) {
        const auto iterator{eventToRequest.find(detection.eventID)};
        if (iterator == eventToRequest.end()) {
            throw std::runtime_error("OptiX output contains a detection for an unknown event ID");
        }
        const auto requestIndex{iterator->second};
        eventOutputs.at(requestIndex).detections.emplace_back(detection);
        ++detectionCounts.at(requestIndex);
    }
    for (std::size_t i {}; i < requests.size(); i++) {
        if (!eventStatisticsSeen.at(i)) {
            throw std::runtime_error("OptiX output is missing event statistics");
        }
        const auto& eventStatistics{eventOutputs.at(i).statistics};
        if ((eventStatistics.validFields & StatisticFieldBit(PhotonTransportStatisticField::Detected)) != 0U &&
            eventStatistics.detectedCount != detectionCounts.at(i)) {
            throw std::runtime_error("OptiX event detected count does not match compacted hits");
        }
        requests.at(i).promise->set_value(std::move(eventOutputs.at(i)));
    }

    {
        std::lock_guard lock{fMutex};
        fRunStatistics.detectedCount += batchStatistics.detectedCount;
        fRunStatistics.absorbedCount += batchStatistics.absorbedCount;
        fRunStatistics.escapedCount += batchStatistics.escapedCount;
        fRunStatistics.truncatedCount += batchStatistics.truncatedCount;
        fRunStatistics.maxBounceCount = std::max(fRunStatistics.maxBounceCount, batchStatistics.maxBounceCount);
        fRunStatistics.invalidStateCount += batchStatistics.invalidStateCount;
        fRunStatistics.zeroStepCount += batchStatistics.zeroStepCount;
        fRunStatistics.validFields |= batchStatistics.validFields;
        if ((batchStatistics.validFields & StatisticFieldBit(PhotonTransportStatisticField::TransportTime)) != 0U) {
            fRunStatistics.transportTimeMs += elapsed;
        }
        fPerformance.Accumulate(transportOutput.performance);
        ++fBatchStatistics.batchCount;
        fBatchStatistics.eventCount += requests.size();
        fBatchStatistics.photonCount += photonCount;
        fBatchStatistics.maxBatchEventCount =
            std::max<std::uint64_t>(fBatchStatistics.maxBatchEventCount, requests.size());
        fBatchStatistics.maxBatchPhotonCount =
            std::max<std::uint64_t>(fBatchStatistics.maxBatchPhotonCount, photonCount);
        fBatchStatistics.minBatchPhotonCount =
            fBatchStatistics.batchCount == 1 ?
                photonCount :
                std::min<std::uint64_t>(fBatchStatistics.minBatchPhotonCount, photonCount);
        fBatchStatistics.performance = fPerformance;
    }
}

auto OpticalBatchScheduler::ProcessBatches() -> void {
#ifdef G4GO_ENABLE_OPTIX
    struct InFlightBatch {
        std::vector<TransportRequest> requests{};
        std::size_t photonCount{};
    };

    std::vector<TransportRequest> requests{};
    std::optional<InFlightBatch> inFlightBatch{};
    try {
        fPhotonTransport = std::make_unique<OptiXTransportHost>(fConfiguration);
        fPhotonTransport->PrepareScene(fScene);
        {
            std::lock_guard lock{fMutex};
            fReady = true;
        }
        fCondition.notify_all();

        for (;;) {
            if (inFlightBatch.has_value()) {
                const auto waitStart{fConfiguration.enablePerformanceDiagnostics ?
                                         std::chrono::steady_clock::now() :
                                         std::chrono::steady_clock::time_point{}};
                auto output{fPhotonTransport->CompleteOldestBatch()};
                if (fConfiguration.enablePerformanceDiagnostics) {
                    std::lock_guard lock{fMutex};
                    fBatchStatistics.schedulerGpuWaitMs +=
                        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - waitStart}.count();
                }
                CompleteBatch(inFlightBatch->requests, inFlightBatch->photonCount, std::move(output));
                inFlightBatch.reset();
                continue;
            }

            requests.clear();
            std::size_t photonCount{};
            {
                std::unique_lock lock{fMutex};
                if (fPendingRequests.empty() && !fStopRequested) {
                    const auto waitStart{fConfiguration.enablePerformanceDiagnostics ?
                                             std::chrono::steady_clock::now() :
                                             std::chrono::steady_clock::time_point{}};
                    fCondition.wait(lock, [this] { return fStopRequested || !fPendingRequests.empty(); });
                    if (fConfiguration.enablePerformanceDiagnostics) {
                        fBatchStatistics.schedulerInputWaitMs +=
                            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - waitStart}
                                .count();
                    }
                }
                if (fPendingRequests.empty() && fStopRequested) {
                    break;
                }

                const auto deadline{fPendingRequests.at(0).queuedAt +
                                    std::chrono::milliseconds{fConfiguration.batchCollectionTimeoutMs}};
                while (photonCount < fConfiguration.targetPhotonsPerBatch &&
                       requests.size() < fConfiguration.maxEventsPerBatch) {
                    if (fPendingRequests.empty()) {
                        if (fStopRequested || photonCount == 0 || [&] {
                                const auto waitStart{fConfiguration.enablePerformanceDiagnostics ?
                                                         std::chrono::steady_clock::now() :
                                                         std::chrono::steady_clock::time_point{}};
                                const auto ready{fCondition.wait_until(
                                    lock, deadline, [this] { return fStopRequested || !fPendingRequests.empty(); })};
                                if (fConfiguration.enablePerformanceDiagnostics) {
                                    fBatchStatistics.schedulerInputWaitMs +=
                                        std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() -
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
                    photonCount += static_cast<std::size_t>(request.submission.photonCount);
                    fQueuedPhotonCount -= static_cast<std::size_t>(request.submission.photonCount);
                    --fQueuedSubmissionCount;
                    if (fConfiguration.enablePerformanceDiagnostics) {
                        fPerformance.schedulerQueueWaitMs +=
                            std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() -
                                                                      request.queuedAt}
                                .count();
                        ++fBatchStatistics.totalQueueWaitCount;
                    }
                    requests.emplace_back(std::move(request));
                }
            }
            fCondition.notify_all();
            if (requests.empty()) {
                continue;
            }

            const auto flattenStart{fConfiguration.enablePerformanceDiagnostics ?
                                        std::chrono::steady_clock::now() :
                                        std::chrono::steady_clock::time_point{}};
            fBatchBuffer.clear();
            std::vector<std::uint32_t> batchEventIDs{};
            batchEventIDs.reserve(requests.size());
            std::vector<std::uint32_t> emissionEventIndices{};
            std::unordered_map<std::uint32_t, std::uint32_t> eventIndices{};
            eventIndices.reserve(requests.size());
            for (const auto& request : requests) {
                const auto eventID{request.submission.eventID};
                const auto eventIndex{static_cast<std::uint32_t>(batchEventIDs.size())};
                if (!eventIndices.emplace(eventID, eventIndex).second) {
                    throw std::invalid_argument("optical batch contains duplicate event IDs");
                }
                batchEventIDs.emplace_back(eventID);
                for (const auto& emission : request.submission.emissions) {
                    if (emission.eventID != eventID) {
                        throw std::invalid_argument("optical emission event ID does not match its "
                                                    "submission");
                    }
                    fBatchBuffer.emplace_back(emission);
                    emissionEventIndices.emplace_back(eventIndex);
                }
            }
            if (fConfiguration.enablePerformanceDiagnostics) {
                const auto flattenMs{
                    std::chrono::duration<double, std::milli>{std::chrono::steady_clock::now() - flattenStart}.count()};
                std::lock_guard lock{fMutex};
                fPerformance.schedulerBatchFlattenMs += flattenMs;
                fPerformance.schedulerBatchFlattenBytes +=
                    static_cast<std::uint64_t>(fBatchBuffer.size() * sizeof(OpticalEmission));
                fBatchStatistics.performance = fPerformance;
            }
            const PhotonTransportBatch batch{fBatchBuffer, batchEventIDs, emissionEventIndices};
            fPhotonTransport->EnqueueEmissions(fScene, batch);
            inFlightBatch.emplace(InFlightBatch{std::move(requests), photonCount});
        }
        fPhotonTransport.reset();
    } catch (...) {
        const auto error{std::current_exception()};
        for (auto& request : requests) {
            try {
                request.promise->set_exception(error);
            } catch (const std::future_error&) {}
        }
        if (inFlightBatch.has_value()) {
            for (auto& request : inFlightBatch->requests) {
                try {
                    request.promise->set_exception(error);
                } catch (const std::future_error&) {}
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

auto OpticalBatchScheduler::FailPendingRequests(std::exception_ptr error) -> void {
    std::deque<TransportRequest> pending{};
    {
        std::lock_guard lock{fMutex};
        pending.swap(fPendingRequests);
        fQueuedPhotonCount = 0;
        fQueuedSubmissionCount = 0;
    }
    for (auto& request : pending) {
        try {
            request.promise->set_exception(error);
        } catch (const std::future_error&) {}
    }
}

} // namespace G4GO::Optical
