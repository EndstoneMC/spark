#include <windows.h>

#include <cstdlib>

extern "C" __declspec(dllexport) void sparkAllocationFixtureRun(volatile LONG *running)
{
    while (::InterlockedCompareExchange(running, 1, 1) == 1) {
        void *pointer = std::malloc(256);
        if (pointer != nullptr) {
            static_cast<volatile unsigned char *>(pointer)[0] = 1;
            std::free(pointer);
        }
        ::SwitchToThread();
    }
}
