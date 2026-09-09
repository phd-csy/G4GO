#pragma once

#include "G4Allocator.hh"
#include "G4QuasiOpticalData.hh"
#include "G4VAuxiliaryTrackInformation.hh"

#include <cstddef>

class G4GOQuasiScintillationTrackInfo final : public G4VAuxiliaryTrackInformation {
public:
    G4GOQuasiScintillationTrackInfo(const G4QuasiOpticalData& data, G4double scintTime, G4double riseTime,
                                    G4int componentIndex);
    ~G4GOQuasiScintillationTrackInfo() override = default;

    auto operator new(size_t) -> void*;
    auto operator delete(void* pointer) -> void;

    G4GOQuasiScintillationTrackInfo(const G4GOQuasiScintillationTrackInfo&) = default;
    auto operator=(const G4GOQuasiScintillationTrackInfo&) -> G4GOQuasiScintillationTrackInfo& = default;

    auto Print() const -> void override;

    auto QuasiOpticalData() const -> G4QuasiOpticalData { return fQuasiOpticalData; }
    auto ScintillationTime() const -> G4double { return fScintillationTime; }
    auto RiseTime() const -> G4double { return fRiseTime; }
    auto ComponentIndex() const -> G4int { return fComponentIndex; }

    static auto Cast(const G4VAuxiliaryTrackInformation* information) -> G4GOQuasiScintillationTrackInfo*;

private:
    G4QuasiOpticalData fQuasiOpticalData;
    G4double fScintillationTime;
    G4double fRiseTime;
    G4int fComponentIndex;
};

auto G4GOQuasiScintillationTrackInfoAllocator() -> G4Allocator<G4GOQuasiScintillationTrackInfo>*&;

inline auto G4GOQuasiScintillationTrackInfo::operator new(size_t) -> void* {
    auto& allocator{G4GOQuasiScintillationTrackInfoAllocator()};
    if (allocator == nullptr) {
        allocator = new G4Allocator<G4GOQuasiScintillationTrackInfo>;
    }
    return allocator->MallocSingle();
}

inline auto G4GOQuasiScintillationTrackInfo::operator delete(void* pointer) -> void {
    G4GOQuasiScintillationTrackInfoAllocator()->FreeSingle(static_cast<G4GOQuasiScintillationTrackInfo*>(pointer));
}
