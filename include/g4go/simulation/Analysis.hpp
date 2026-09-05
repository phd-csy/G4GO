#pragma once

#include "g4go/optical/PhotonTransport.hpp"

#include <cstddef>
#include <deque>
#include <vector>

namespace G4GO::Simulation {

struct CrystalHitOutput {
    int fEventID{};
    int fModuleID{};
    float fEnergyDeposit{};
    int fGeneratedPhotonCount{};
};

struct SensorHitOutput {
    int fEventID{};
    int fSensorID{};
    float fTimeOfFlight{};
};

class Analysis final {
public:
    auto Enqueue(
        std::vector<CrystalHitOutput> crystalHits,
        std::vector<SensorHitOutput> sensorHits,
        G4GO::Optical::PhotonTransportFuture transportFuture) -> void;
    auto WriteReadyEvents() -> void;
    auto WaitAndWriteNextEvent() -> void;
    auto Flush() -> void;
    auto PendingCount() const -> std::size_t { return fPendingEvents.size(); }

private:
    struct PendingEvent {
        std::vector<CrystalHitOutput> fCrystalHits{};
        std::vector<SensorHitOutput> fSensorHits{};
        G4GO::Optical::PhotonTransportFuture fTransportFuture{};
    };

    auto ProcessReadyEvents(bool waitForFrontEvent) -> void;
    auto WriteEvent(PendingEvent event) -> void;

    std::deque<PendingEvent> fPendingEvents{};
};

} // namespace G4GO::Simulation
