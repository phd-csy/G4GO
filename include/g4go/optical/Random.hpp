#pragma once

#include <array>
#include <cstdint>

namespace G4GO::Optical {

namespace detail {

struct PhiloxWords {
    std::uint32_t fX0{};
    std::uint32_t fX1{};
    std::uint32_t fX2{};
    std::uint32_t fX3{};
};

inline auto MultiplyHighLow(std::uint32_t left, std::uint32_t right)
    -> std::array<std::uint32_t, 2> {
    const auto product{static_cast<std::uint64_t>(left) * right};
    return {
        static_cast<std::uint32_t>(product),
        static_cast<std::uint32_t>(product >> 32),
    };
}

inline auto Philox4x32(PhiloxWords counter,
                       std::uint32_t key0,
                       std::uint32_t key1) -> PhiloxWords {
    constexpr std::uint32_t multiplier0{0xD2511F53U};
    constexpr std::uint32_t multiplier1{0xCD9E8D57U};
    constexpr std::uint32_t keyStep0{0x9E3779B9U};
    constexpr std::uint32_t keyStep1{0xBB67AE85U};

    for (auto round{0}; round < 10; ++round) {
        const auto product0{MultiplyHighLow(multiplier0, counter.fX0)};
        const auto product1{MultiplyHighLow(multiplier1, counter.fX2)};
        counter = {
            product1[1] ^ counter.fX1 ^ key0,
            product1[0],
            product0[1] ^ counter.fX3 ^ key1,
            product0[0],
        };
        key0 += keyStep0;
        key1 += keyStep1;
    }
    return counter;
}

} // namespace detail

inline auto Uniform(std::uint64_t seed,
                    std::uint64_t photonID,
                    std::uint32_t bounce,
                    std::uint32_t process,
                    std::uint32_t draw) -> float {
    const auto result{detail::Philox4x32(
        {
            static_cast<std::uint32_t>(photonID),
            static_cast<std::uint32_t>(photonID >> 32),
            bounce,
            (process << 16U) ^ draw,
        },
        static_cast<std::uint32_t>(seed),
        static_cast<std::uint32_t>(seed >> 32))};
    const auto word{draw % 4U == 0U ? result.fX0 : draw % 4U == 1U ? result.fX1 :
                                               draw % 4U == 2U     ? result.fX2 :
                                                                     result.fX3};
    return (static_cast<float>(word) + 0.5F) / 4294967296.0F;
}

} // namespace G4GO::Optical
