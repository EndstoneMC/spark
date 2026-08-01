#include <sys/types.h>
#include <unistd.h>

extern "C" pid_t sparkElfHookFixtureGetpid()
{
    return ::getpid();
}
