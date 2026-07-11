#include "sampler/symbolicate.h"

#include <string_view>

#if defined(_WIN32)
#include <cpptrace/cpptrace.hpp>
#else
#include <cstdlib>

#include <cxxabi.h>
#include <dlfcn.h>
#endif

namespace spark {

namespace {

std::string basename(const std::string &path)
{
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string hex(std::uint64_t value)
{
    static const char *digits = "0123456789abcdef";
    if (value == 0) {
        return "0x0";
    }
    char buf[19];
    int i = 18;
    while (value != 0 && i > 1) {
        buf[i--] = digits[value & 0xf];
        value >>= 4;
    }
    buf[i--] = 'x';
    buf[i] = '0';
    return std::string(buf + i, 18 - i + 1);
}

}  // namespace

#if !defined(_WIN32)

// Linux: resolve against the live process with dladdr. This reads only the dynamic
// symbol table (never DWARF), so it is safe on a huge stripped binary like BDS where
// cpptrace's libdwarf backend can fault, and it degrades gracefully to module+RVA for
// unknown addresses.
std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> resolveFrames(const ModuleTable &modules,
                                                                        const std::vector<FrameKey> &keys)
{
    std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> out;
    out.reserve(keys.size());

    for (const FrameKey &key : keys) {
        ResolvedFrame rf;
        rf.class_name = basename(modules.path(key.module));

        Dl_info info{};
        if (key.raw_address != 0 && dladdr(reinterpret_cast<void *>(key.raw_address), &info) != 0 &&
            info.dli_sname != nullptr) {
            int status = 0;
            char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
            rf.method_name = status == 0 && demangled != nullptr ? demangled : info.dli_sname;
            std::free(demangled);
            if (info.dli_fname != nullptr && info.dli_fname[0] != '\0') {
                rf.class_name = basename(info.dli_fname);
            }
        }
        else {
            rf.method_name = hex(key.rva);
        }
        out.emplace(key, std::move(rf));
    }
    return out;
}

bool isSleepFrame(std::uint64_t raw_address)
{
    if (raw_address == 0) {
        return false;
    }
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(raw_address), &info) == 0 || info.dli_sname == nullptr) {
        return false;
    }
    std::string_view name = info.dli_sname;
    for (std::string_view sub : {std::string_view("nanosleep"), std::string_view("futex"),
                                 std::string_view("epoll_wait"), std::string_view("epoll_pwait"),
                                 std::string_view("cond_wait"), std::string_view("cond_timedwait")}) {
        if (name.find(sub) != std::string_view::npos) {
            return true;
        }
    }
    for (std::string_view exact : {std::string_view("poll"), std::string_view("ppoll"),
                                   std::string_view("select"), std::string_view("pselect"),
                                   std::string_view("sched_yield"), std::string_view("usleep")}) {
        if (name == exact) {
            return true;
        }
    }
    return false;
}

#else

// Windows: cpptrace resolves via DbgHelp/PDB (BDS ships a PDB), giving real names.
std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> resolveFrames(const ModuleTable &modules,
                                                                        const std::vector<FrameKey> &keys)
{
    std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> out;
    out.reserve(keys.size());

    for (const FrameKey &key : keys) {
        cpptrace::object_frame frame;
        frame.raw_address = static_cast<cpptrace::frame_ptr>(key.raw_address);
        frame.object_address = static_cast<cpptrace::frame_ptr>(key.rva);
        frame.object_path = modules.path(key.module);

        cpptrace::object_trace trace;
        trace.frames.push_back(frame);
        cpptrace::stacktrace resolved = trace.resolve();

        ResolvedFrame rf;
        rf.class_name = basename(modules.path(key.module));
        const cpptrace::stacktrace_frame *pick = nullptr;
        for (const cpptrace::stacktrace_frame &f : resolved.frames) {
            if (!f.symbol.empty()) {
                pick = &f;
                break;
            }
        }
        if (pick != nullptr) {
            rf.method_name = pick->symbol;
            if (pick->line.has_value()) {
                rf.line = static_cast<std::int32_t>(pick->line.value());
            }
        }
        else {
            rf.method_name = hex(key.rva);
        }
        out.emplace(key, std::move(rf));
    }
    return out;
}

bool isSleepFrame(std::uint64_t /*raw_address*/)
{
    return false;  // TODO(windows): detect sleep/wait frames via symbol name
}

#endif

}  // namespace spark
