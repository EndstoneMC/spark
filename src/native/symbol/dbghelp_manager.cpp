#include "native/symbol/dbghelp_manager.h"

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace spark {
namespace {

std::mutex GMutex;
std::size_t GReferences = 0;

}  // namespace

bool retainDbgHelp()
{
    std::scoped_lock lock(GMutex);
    if (GReferences == 0) {
        SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS |
                      SYMOPT_LOAD_LINES);
        if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE)) {
            return false;
        }
    }
    ++GReferences;
    return true;
}

void releaseDbgHelp()
{
    std::scoped_lock lock(GMutex);
    if (GReferences == 0) {
        return;
    }
    --GReferences;
    if (GReferences == 0) {
        SymCleanup(GetCurrentProcess());
    }
}

std::mutex &dbgHelpMutex()
{
    return GMutex;
}

}  // namespace spark
