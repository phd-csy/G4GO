#include "g4go/optical/geant4/G4GOQuasiScintillationTrackInfo.hpp"

#include "G4ios.hh"
#include "tls.hh"

auto G4GOQuasiScintillationTrackInfoAllocator() -> G4Allocator<G4GOQuasiScintillationTrackInfo>*& {
    G4ThreadLocalStatic G4Allocator<G4GOQuasiScintillationTrackInfo>* allocator{};
    return allocator;
}

G4GOQuasiScintillationTrackInfo::G4GOQuasiScintillationTrackInfo(const G4QuasiOpticalData& data,
                                                                 G4double scintillationTime, G4double riseTime,
                                                                 G4int componentIndex) :
    fQuasiOpticalData{data},
    fScintillationTime{scintillationTime},
    fRiseTime{riseTime},
    fComponentIndex{componentIndex} {}

auto G4GOQuasiScintillationTrackInfo::Print() const -> void {
    G4cout << "G4GO scintillation component " << fComponentIndex << ", photons=" << fQuasiOpticalData.num_photons
           << G4endl;
}

auto G4GOQuasiScintillationTrackInfo::Cast(const G4VAuxiliaryTrackInformation* information)
    -> G4GOQuasiScintillationTrackInfo* {
    return dynamic_cast<G4GOQuasiScintillationTrackInfo*>(const_cast<G4VAuxiliaryTrackInformation*>(information));
}
