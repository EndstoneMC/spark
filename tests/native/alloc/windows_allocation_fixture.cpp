#include <malloc.h>
#include <windows.h>

#include <cstddef>
#include <cstdlib>

extern "C" {
__declspec(dllimport) void *__cdecl _malloc_base(std::size_t);
__declspec(dllimport) void *__cdecl _calloc_base(std::size_t, std::size_t);
__declspec(dllimport) void *__cdecl _realloc_base(void *, std::size_t);
}

extern "C" __declspec(dllexport) void sparkAllocationFixtureOnce()
{
    void *pointer = std::malloc(256);
    if (pointer != nullptr) {
        static_cast<volatile unsigned char *>(pointer)[0] = 1;
        std::free(pointer);
    }
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureRetain(std::size_t size)
{
    void *pointer = std::malloc(size);
    if (pointer != nullptr && size != 0) {
        static_cast<volatile unsigned char *>(pointer)[0] = 1;
    }
    return pointer;
}

extern "C" __declspec(dllexport) void sparkAllocationFixtureRelease(void *pointer)
{
    std::free(pointer);
}

extern "C" __declspec(dllexport) void sparkAllocationFixtureRun(volatile LONG *running)
{
    while (::InterlockedCompareExchange(running, 1, 1) == 1) {
        sparkAllocationFixtureOnce();
        ::SwitchToThread();
    }
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureHeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    return ::HeapAlloc(heap, flags, size);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureHeapReAlloc(HANDLE heap, DWORD flags, void *pointer,
                                                                         SIZE_T size)
{
    return ::HeapReAlloc(heap, flags, pointer, size);
}

extern "C" __declspec(dllexport) BOOL sparkAllocationFixtureHeapFree(HANDLE heap, DWORD flags, void *pointer)
{
    return ::HeapFree(heap, flags, pointer);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureMalloc(std::size_t size)
{
    return std::malloc(size);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureCalloc(std::size_t count, std::size_t size)
{
    return std::calloc(count, size);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureRealloc(void *pointer, std::size_t size)
{
    return std::realloc(pointer, size);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureRecalloc(void *pointer, std::size_t count,
                                                                      std::size_t size)
{
    return _recalloc(pointer, count, size);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureAlignedMalloc(std::size_t size, std::size_t alignment)
{
    return _aligned_malloc(size, alignment);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureAlignedRealloc(void *pointer, std::size_t size,
                                                                            std::size_t alignment)
{
    return _aligned_realloc(pointer, size, alignment);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureAlignedRecalloc(void *pointer, std::size_t count,
                                                                             std::size_t size, std::size_t alignment)
{
    return _aligned_recalloc(pointer, count, size, alignment);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureAlignedOffsetMalloc(std::size_t size,
                                                                                 std::size_t alignment,
                                                                                 std::size_t offset)
{
    return _aligned_offset_malloc(size, alignment, offset);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureAlignedOffsetRealloc(void *pointer, std::size_t size,
                                                                                  std::size_t alignment,
                                                                                  std::size_t offset)
{
    return _aligned_offset_realloc(pointer, size, alignment, offset);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureAlignedOffsetRecalloc(void *pointer, std::size_t count,
                                                                                   std::size_t size,
                                                                                   std::size_t alignment,
                                                                                   std::size_t offset)
{
    return _aligned_offset_recalloc(pointer, count, size, alignment, offset);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureMallocBase(std::size_t size)
{
    return _malloc_base(size);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureCallocBase(std::size_t count, std::size_t size)
{
    return _calloc_base(count, size);
}

extern "C" __declspec(dllexport) void *sparkAllocationFixtureReallocBase(void *pointer, std::size_t size)
{
    return _realloc_base(pointer, size);
}

extern "C" __declspec(dllexport) void sparkAllocationFixtureFree(void *pointer)
{
    std::free(pointer);
}

extern "C" __declspec(dllexport) void sparkAllocationFixtureAlignedFree(void *pointer)
{
    _aligned_free(pointer);
}
