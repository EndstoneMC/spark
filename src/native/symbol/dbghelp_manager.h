#ifndef SPARK_NATIVE_SYMBOL_DBGHELP_MANAGER_H
#define SPARK_NATIVE_SYMBOL_DBGHELP_MANAGER_H

#include <mutex>

namespace spark {

bool retainDbgHelp();
void releaseDbgHelp();
std::mutex &dbgHelpMutex();

class DbgHelpReference {
public:
    DbgHelpReference() : initialized_(retainDbgHelp()) {}
    ~DbgHelpReference()
    {
        if (initialized_) {
            releaseDbgHelp();
        }
    }

    DbgHelpReference(const DbgHelpReference &) = delete;
    DbgHelpReference &operator=(const DbgHelpReference &) = delete;

    bool initialized() const { return initialized_; }

private:
    bool initialized_;
};

}  // namespace spark

#endif  // SPARK_NATIVE_SYMBOL_DBGHELP_MANAGER_H
