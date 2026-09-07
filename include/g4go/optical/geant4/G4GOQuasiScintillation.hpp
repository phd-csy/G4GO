// ********************************************************************
// * License and Disclaimer                                           *
// *                                                                  *
// * The  Geant4 software is  copyright of the Copyright Holders  of *
// * the  Geant4 Collaboration.  It is provided  under  the  terms  *
// * and  conditions  of  the  Geant4 Software License,  included in *
// * the file  LICENSE  and available at  http://cern.ch/geant4/license .  *
// * These include a list of copyright holders.                       *
// *                                                                  *
// * Neither the authors of this software system, nor their employing *
// * institutes,nor the agencies providing financial support for this *
// * work  make  any representation or  warranty, express or  implied, *
// * regarding  this  software system or assume any liability for its *
// * use.  Please see the license in the file  LICENSE  and URL above *
// * for the full disclaimer and the limitation of liability.         *
// *                                                                  *
// * This  code  implementation is the result of  the  scientific and *
// * technical work of the GEANT4 collaboration.                      *
// * By using,  copying,  modifying or  distributing the software (or *
// * any work based  on the software)  you  agree  to acknowledge its *
// * use  in  resulting  scientific  publications,  and indicate your *
// * acceptance of all terms of the Geant4 Software license.          *
// ********************************************************************

#pragma once

#include "G4QuasiScintillation.hh"

class G4Step;
class G4Track;
class G4VParticleChange;

// Source calculation follows Geant4 11.4.2 G4QuasiScintillation.  The GPU
// variant emits only compact quasi tracks, with an explicit component index;
// it never enters the regular optical-photon secondary loop.
class G4GOQuasiScintillation final : public G4QuasiScintillation {
public:
    explicit G4GOQuasiScintillation(const G4String& processName = "G4GOQuasiScintillation",
                                    G4ProcessType type = fElectromagnetic);
    ~G4GOQuasiScintillation() override = default;

    G4GOQuasiScintillation(const G4GOQuasiScintillation&) = delete;
    auto operator=(const G4GOQuasiScintillation&) -> G4GOQuasiScintillation& = delete;

    auto PostStepDoIt(const G4Track& track, const G4Step& step) -> G4VParticleChange* override;
    auto AtRestDoIt(const G4Track& track, const G4Step& step) -> G4VParticleChange* override;
};
