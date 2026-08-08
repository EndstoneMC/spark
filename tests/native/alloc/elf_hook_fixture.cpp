#include <unistd.h>

#include <sys/types.h>

extern "C" pid_t sparkElfHookFixtureGetpid()
{
    return ::getpid();
}
