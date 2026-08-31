#pragma once

#include "g4go/optical/PhotonTransport.hpp"

#include <cstddef>
#include <deque>
#include <vector>

namespace G4GO::Simulation {

struct CrystalHitOutput {
    int fEventID{};
    int fModuleID{};
    double fEnergyDeposit{};
};

class Analysis final {
public:
    auto Enqueue(
        std::vector<CrystalHitOutput> crystalHits,
        G4GO::Optical::PhotonTransportFuture transportResult) -> void;
    auto WriteReadyEvents() -> void;
    auto WaitAndWriteNextEvent() -> void;
    auto Flush() -> void;
    auto PendingCount() const -> std::size_t { return fPendingEvents.size(); }

private:
    struct PendingEvent {
        std::vector<CrystalHitOutput> fCrystalHits{};
        G4GO::Optical::PhotonTransportFuture fTransportResult{};
    };

    auto ProcessReadyEvents(bool waitForFrontEvent) -> void;
    auto WriteEvent(PendingEvent event) -> void;

    std::deque<PendingEvent> fPendingEvents{};
};

} // namespace G4GO::Simulation
