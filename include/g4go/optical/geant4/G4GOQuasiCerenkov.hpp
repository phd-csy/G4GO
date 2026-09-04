#pragma once

#include "G4QuasiCerenkov.hh"

class G4Step;
class G4Track;
class G4VParticleChange;

// Compact Cerenkov source process used by the GPU backend.  The base Geant4
// quasi process computes the source count and metadata; this wrapper also
// consumes the random draws that native G4Cerenkov would use while creating
// individual photons, keeping the charged-particle source stream aligned.
class G4GOQuasiCerenkov final : public G4QuasiCerenkov {
public:
    explicit G4GOQuasiCerenkov(
        const G4String& processName = "G4GOQuasiCerenkov",
        G4ProcessType type = fElectromagnetic);
    ~G4GOQuasiCerenkov() override = default;

    G4GOQuasiCerenkov(const G4GOQuasiCerenkov&) = delete;
    auto operator=(const G4GOQuasiCerenkov&)
        -> G4GOQuasiCerenkov& = delete;

    auto PostStepDoIt(const G4Track& track, const G4Step& step)
        -> G4VParticleChange* override;
};
