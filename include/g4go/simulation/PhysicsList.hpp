#pragma once

class G4VModularPhysicsList;

namespace G4GO::Simulation {

// Registers Geant4's built-in quasi optical processes used by the offload
// path.  The process implementations stay in Geant4; G4GO only selects and
// configures them so that they emit compact quasi-photon metadata.
auto RegisterOpticalOffloadProcesses(G4VModularPhysicsList& physicsList)
    -> void;

} // namespace G4GO::Simulation
