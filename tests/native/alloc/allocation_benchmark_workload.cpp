#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace {

std::size_t allocationSize(std::size_t kind, std::size_t iteration)
{
    switch (kind) {
    case 0:
        return 16 + iteration % 49;
    case 1:
    case 4:
    case 5:
    case 6:
        return 64 + iteration % 193;
    case 2:
        return 256 + iteration % 769;
    case 3:
        switch (iteration % 3) {
        case 0:
            return 16 + iteration % 49;
        case 1:
            return 64 + iteration % 193;
        default:
            return 256 + iteration % 769;
        }
    default:
        return 0;
    }
}

void touchAllocation(void *pointer, std::size_t bytes, std::size_t iteration)
{
    auto *memory = static_cast<volatile unsigned char *>(pointer);
    memory[0] = static_cast<unsigned char>(iteration);
    if (bytes > 1) {
        memory[bytes - 1] = static_cast<unsigned char>(iteration >> 8);
    }
}

}  // namespace

extern "C" void spark_benchmark_workload(std::size_t kind, std::size_t operations, std::uint64_t *requested_bytes)
{
    if (requested_bytes == nullptr) {
        return;
    }
    *requested_bytes = 0;

    if (kind == 5) {
        if (operations < 2) {
            *requested_bytes = std::numeric_limits<std::uint64_t>::max();
            return;
        }

        std::size_t bytes = allocationSize(5, 0);
        void *pointer = ::malloc(bytes);
        if (pointer == nullptr) {
            return;
        }
        *requested_bytes += bytes;
        touchAllocation(pointer, bytes, 0);

        for (std::size_t i = 0; i < operations - 2; ++i) {
            bytes = allocationSize(5, i + 1);
            void *replacement = ::realloc(pointer, bytes);
            if (replacement == nullptr) {
                break;
            }
            pointer = replacement;
            *requested_bytes += bytes;
            touchAllocation(pointer, bytes, i + 1);
        }
        ::free(pointer);
        return;
    }

    if ((kind <= 4 || kind == 6) && (operations % 2) != 0) {
        *requested_bytes = std::numeric_limits<std::uint64_t>::max();
        return;
    }

    for (std::size_t i = 0; i < operations / 2; ++i) {
        std::size_t bytes = allocationSize(kind, i);
        void *pointer = nullptr;
        if (kind == 4) {
            pointer = ::calloc(1, bytes);
        }
        else if (kind == 6) {
            bytes = (bytes + 63) & ~std::size_t{63};
            pointer = ::aligned_alloc(64, bytes);
        }
        else if (kind <= 3) {
            pointer = ::malloc(bytes);
        }
        if (pointer == nullptr) {
            break;
        }
        *requested_bytes += bytes;
        touchAllocation(pointer, bytes, i);
        ::free(pointer);
    }
}
