#pragma once

#include "g4go/optical/Types.hpp"

#include <cstddef>
#include <deque>
#include <vector>

namespace G4GO::Simulation {

struct CrystalHitRow {
    int fEventID{};
    int fModuleID{};
    double fEnergyDeposit{};
};

struct PendingEventOutput {
    int fEventID{};
    std::vector<CrystalHitRow> fCrystalHits{};
    G4GO::Optical::EventTransportFuture fTransport{};
};

class EventOutputQueue final {
public:
    auto Submit(PendingEventOutput output) -> void;
    auto DrainReady(bool waitForOne) -> void;
    auto Flush() -> void;
    auto Size() const -> std::size_t { return fPending.size(); }

private:
    auto Write(PendingEventOutput output) -> void;

    std::deque<PendingEventOutput> fPending{};
};

} // namespace G4GO::Simulation
