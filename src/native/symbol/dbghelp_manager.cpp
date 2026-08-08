#include "native/symbol/dbghelp_manager.h"

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace spark {
namespace {

std::mutex g_mutex;
std::size_t g_references = 0;

}  // namespace

bool retainDbgHelp()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_references == 0) {
        SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS |
                      SYMOPT_LOAD_LINES);
        if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
            return false;
        }
    }
    ++g_references;
    return true;
}

void releaseDbgHelp()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_references == 0) {
        return;
    }
    --g_references;
    if (g_references == 0) {
        SymCleanup(GetCurrentProcess());
    }
}

std::mutex &dbgHelpMutex()
{
    return g_mutex;
}

}  // namespace spark
