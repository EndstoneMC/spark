#ifndef ENDSTONE_SPARK_BYTE_SAMPLER_H
#define ENDSTONE_SPARK_BYTE_SAMPLER_H

#include <cstdint>

namespace spark {

// Per-thread byte-based systematic sampling state. Unbiased, low-variance, O(1) per allocation.
struct ByteSamplingState {
    std::uint64_t generation = 0;
    std::uint64_t bytes_until_sample = 1;
    std::uint64_t random_state = 0;
};

inline std::uint64_t mixSamplingSeed(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

inline std::uint64_t nextSamplingRandom(ByteSamplingState &state) noexcept
{
    std::uint64_t value = state.random_state;
    if (value == 0) {
        value = 0x9e3779b97f4a7c15ULL;
    }
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    state.random_state = value;
    return value * 0x2545f4914f6cdd1dULL;
}

inline std::uint64_t initialByteSamplingOffset(ByteSamplingState &state, std::uint64_t interval) noexcept
{
    if (interval <= 1) {
        return 1;
    }

    // Rejection avoids modulo bias, including for the largest supported
    // interval. The returned byte offset is in [1, interval].
    const std::uint64_t reject_below = (std::uint64_t{0} - interval) % interval;
    std::uint64_t value = 0;
    do {
        value = nextSamplingRandom(state);
    } while (value < reject_below);
    return value % interval + 1;
}

inline void resetByteSamplingState(ByteSamplingState &state, std::uint64_t generation, std::uint64_t seed,
                                   std::uint64_t interval) noexcept
{
    state.generation = generation;
    state.random_state = mixSamplingSeed(seed);
    state.bytes_until_sample = initialByteSamplingOffset(state, interval);
}

inline std::uint64_t consumeSampledBytes(ByteSamplingState &state, std::uint64_t requested_bytes,
                                         std::uint64_t interval) noexcept
{
    if (interval <= 1) {
        state.bytes_until_sample = 1;
        return requested_bytes;
    }

    if (requested_bytes < state.bytes_until_sample) {
        state.bytes_until_sample -= requested_bytes;
        return 0;
    }

    const std::uint64_t after_first = requested_bytes - state.bytes_until_sample;
    const std::uint64_t sample_points = 1 + after_first / interval;
    const std::uint64_t remainder = after_first % interval;
    state.bytes_until_sample = interval - remainder;
    return sample_points;
}

}  // namespace spark

#endif  // ENDSTONE_SPARK_BYTE_SAMPLER_H
