#include "sampler/symbolicate.h"

#include <cctype>
#include <limits>
#include <string_view>

#include "sampler/symbol_guess.h"

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
// clang-format on
#else
#include <climits>
#include <cstdlib>

#include <cxxabi.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace spark {

namespace {

#if defined(_WIN32)
struct SymbolBuffer {
  SYMBOL_INFO info;
  char name[MAX_SYM_NAME];
};

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const auto ac = static_cast<unsigned char>(a[i]);
    const auto bc = static_cast<unsigned char>(b[i]);
    if (std::tolower(ac) != std::tolower(bc)) {
      return false;
    }
  }
  return true;
}

bool startsWithIgnoreCase(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         equalsIgnoreCase(value.substr(0, prefix.size()), prefix);
}

bool isAmbiguousCrtModule(std::string_view module) {
  return equalsIgnoreCase(module, "ucrtbase.dll") ||
         equalsIgnoreCase(module, "vcruntime140.dll") ||
         equalsIgnoreCase(module, "vcruntime140_1.dll") ||
         equalsIgnoreCase(module, "msvcp140.dll") ||
         startsWithIgnoreCase(module, "api-ms-win-crt-");
}

bool isWindowsSystemModule(std::string_view module) {
  return isAmbiguousCrtModule(module) ||
         equalsIgnoreCase(module, "ntdll.dll") ||
         equalsIgnoreCase(module, "kernel32.dll") ||
         equalsIgnoreCase(module, "kernelbase.dll");
}

// SymFromAddr returns the nearest symbol at or below an address. When only a
// DLL export table is available (common for ucrtbase.dll), an unrelated export
// can therefore be reported for a private routine many bytes later. Accept CRT
// names only when DbgHelp knows the symbol extent, or when the address is
// exactly at the exported entry point. Otherwise module+RVA is more honest than
// a false function name such as ucrtbase.dll!wcsrchr.
bool trustworthyWindowsSymbol(std::string_view module,
                              const SYMBOL_INFO &symbol, DWORD64 displacement,
                              SYM_TYPE module_symbol_type) {
  if (!isWindowsSystemModule(module)) {
    return true;
  }

  if (isAmbiguousCrtModule(module)) {
    // When DbgHelp only has a DLL export table, SymFromAddr can attribute a
    // private CRT routine to the preceding exported function. Export entries
    // may also be assigned a synthetic non-zero Size, so Size/displacement
    // alone is not sufficient to prove that the name is correct. Keep CRT
    // names only when richer symbols (PDB/CodeView/etc.) are loaded.
    if (module_symbol_type == SymExport ||
        (symbol.Flags & SYMFLAG_EXPORT) != 0) {
      return false;
    }
    if (symbol.Size != 0) {
      return displacement < symbol.Size;
    }
    return displacement == 0;
  }

  // Apply a broad sanity limit to export-only symbols from the remaining core
  // system DLLs. Real public/PDB function symbols normally carry a useful size.
  if (symbol.Size == 0) {
    return displacement <= 0x4000;
  }
  return displacement < symbol.Size;
}

class DbgHelpSession {
public:
  explicit DbgHelpSession(HANDLE process)
      : process_(process), initialized_(SymInitialize(process, nullptr, TRUE)) {
  }

  ~DbgHelpSession() {
    if (initialized_) {
      SymCleanup(process_);
    }
  }

  bool initialized() const { return initialized_ != FALSE; }

private:
  HANDLE process_;
  BOOL initialized_;
};
#endif

std::string basename(const std::string &path) {
  auto pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string hex(std::uint64_t value) {
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

} // namespace

bool frameMatchesMainModule(std::uint64_t raw_address, std::uint64_t rva,
                            std::uint64_t module_base,
                            std::uint64_t module_size) {
  if (raw_address < module_base || module_size == 0 || rva >= module_size ||
      module_size > (std::numeric_limits<std::uint64_t>::max)() - module_base) {
    return false;
  }
  return raw_address < module_base + module_size &&
         raw_address - module_base == rva;
}

void applySymbolGuessFallback(ResolvedFrame &frame, std::uint64_t rva,
                              bool main_module, std::string_view guess) {
  GuessResult result;
  result.function_rva = rva;
  result.label = guess;
  applySymbolGuessFallback(frame, rva, main_module, result);
}

void applySymbolGuessFallback(ResolvedFrame &frame, std::uint64_t rva,
                              bool main_module, const GuessResult &guess) {
  const std::string address = hex(rva);
  if (frame.method_name.empty()) {
    frame.method_name = address;
  }
  if (!main_module || frame.method_name != address) {
    return;
  }
  if (guess.function_rva != 0) {
    frame.method_name = hex(guess.function_rva);
    frame.guessed_function_rva = guess.function_rva;
  }
  if (!guess.label.empty()) {
    frame.method_name += " (";
    frame.method_name += guess.label;
    frame.method_name += ')';
  }
}

#if !defined(_WIN32)

// Linux: resolve against the live process with dladdr. This reads only the
// dynamic symbol table (never DWARF), so it is safe on a huge stripped binary
// like BDS where cpptrace's libdwarf backend can fault, and it degrades
// gracefully to module+RVA for unknown addresses.
std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash>
resolveFrames(const ModuleTable &modules, const std::vector<FrameKey> &keys) {
  std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> out;
  out.reserve(keys.size());

  char executable_path[PATH_MAX] = {};
  const ssize_t executable_length = ::readlink(
      "/proc/self/exe", executable_path, sizeof(executable_path) - 1);
  const std::string executable_name =
      executable_length > 0
          ? basename(std::string(executable_path,
                                 static_cast<std::size_t>(executable_length)))
          : std::string();
  const std::string executable_full_path =
      executable_length > 0
          ? std::string(executable_path,
                        static_cast<std::size_t>(executable_length))
          : std::string();
  std::vector<std::uint64_t> unresolved_main_rvas;

  for (const FrameKey &key : keys) {
    ResolvedFrame rf;
    const std::string &module_path = modules.path(key.module);
    rf.class_name = basename(module_path);

    Dl_info info{};
    if (key.raw_address != 0 &&
        dladdr(reinterpret_cast<void *>(key.raw_address), &info) != 0 &&
        info.dli_sname != nullptr) {
      int status = 0;
      char *demangled =
          abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
      rf.method_name =
          status == 0 && demangled != nullptr ? demangled : info.dli_sname;
      std::free(demangled);
      if (info.dli_fname != nullptr && info.dli_fname[0] != '\0') {
        rf.class_name = basename(info.dli_fname);
      }
    }

    if (rf.method_name.empty()) {
      rf.method_name = hex(key.rva);
      if (!executable_full_path.empty() &&
          module_path == executable_full_path &&
          rf.class_name == executable_name) {
        unresolved_main_rvas.push_back(key.rva);
      }
    }
    out.emplace(key, std::move(rf));
  }

  const auto guesses = analyzeMainModuleSymbols(unresolved_main_rvas);
  for (const FrameKey &key : keys) {
    if (executable_full_path.empty() ||
        modules.path(key.module) != executable_full_path) {
      continue;
    }
    const auto guess = guesses.find(key.rva);
    if (guess == guesses.end()) {
      continue;
    }
    auto frame = out.find(key);
    if (frame != out.end()) {
      applySymbolGuessFallback(frame->second, key.rva, true, guess->second);
    }
  }
  return out;
}

bool isSleepFrame(std::uint64_t raw_address) {
  if (raw_address == 0) {
    return false;
  }
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(raw_address), &info) == 0 ||
      info.dli_sname == nullptr) {
    return false;
  }
  std::string_view name = info.dli_sname;
  for (std::string_view sub :
       {std::string_view("nanosleep"), std::string_view("futex"),
        std::string_view("epoll_wait"), std::string_view("epoll_pwait"),
        std::string_view("cond_wait"), std::string_view("cond_timedwait")}) {
    if (name.find(sub) != std::string_view::npos) {
      return true;
    }
  }
  for (std::string_view exact :
       {std::string_view("poll"), std::string_view("ppoll"),
        std::string_view("select"), std::string_view("pselect"),
        std::string_view("sched_yield"), std::string_view("usleep")}) {
    if (name == exact) {
      return true;
    }
  }
  return false;
}

#else

// Windows: resolve directly through DbgHelp. Sampling has already stopped and
// the capture session has been cleaned up, so use a short-lived symbol session
// for the export batch.
std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash>
resolveFrames(const ModuleTable &modules, const std::vector<FrameKey> &keys) {
  std::unordered_map<FrameKey, ResolvedFrame, FrameKeyHash> out;
  out.reserve(keys.size());

  HANDLE process = GetCurrentProcess();
  SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME |
                SYMOPT_LOAD_LINES);
  DbgHelpSession session(process);
  HMODULE executable_module = GetModuleHandleW(nullptr);
  MODULEINFO executable_info{};
  const bool have_executable_range =
      executable_module != nullptr &&
      GetModuleInformation(process, executable_module, &executable_info,
                           sizeof(executable_info)) != FALSE;
  const std::uint64_t executable_base =
      have_executable_range
          ? reinterpret_cast<std::uint64_t>(executable_info.lpBaseOfDll)
          : 0;
  std::unordered_map<ModuleId, SYM_TYPE> module_symbol_types;
  module_symbol_types.reserve(modules.size());
  std::vector<std::uint64_t> unresolved_main_rvas;
  unresolved_main_rvas.reserve(keys.size());

  for (const FrameKey &key : keys) {
    ResolvedFrame rf;
    rf.class_name = basename(modules.path(key.module));

    if (session.initialized() && key.raw_address != 0) {
      SymbolBuffer symbol{};
      symbol.info.SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol.info.MaxNameLen = MAX_SYM_NAME;

      SYM_TYPE module_symbol_type = SymNone;
      if (const auto it = module_symbol_types.find(key.module);
          it != module_symbol_types.end()) {
        module_symbol_type = it->second;
      } else {
        IMAGEHLP_MODULE64 module_info{};
        module_info.SizeOfStruct = sizeof(module_info);
        if (SymGetModuleInfo64(process, key.raw_address, &module_info) !=
            FALSE) {
          module_symbol_type = module_info.SymType;
        }
        module_symbol_types.emplace(key.module, module_symbol_type);
      }

      DWORD64 displacement = 0;
      if (SymFromAddr(process, key.raw_address, &displacement, &symbol.info) &&
          trustworthyWindowsSymbol(rf.class_name, symbol.info, displacement,
                                   module_symbol_type)) {
        rf.method_name.assign(symbol.info.Name, symbol.info.NameLen);

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        if (SymGetLineFromAddr64(process, key.raw_address, &line_displacement,
                                 &line)) {
          rf.line = static_cast<std::int32_t>(line.LineNumber);
        }
      }
    }

    if (rf.method_name.empty()) {
      const bool main_module =
          have_executable_range &&
          frameMatchesMainModule(key.raw_address, key.rva, executable_base,
                                 executable_info.SizeOfImage);
      applySymbolGuessFallback(rf, key.rva, main_module,
                               std::string_view{});
      if (main_module) {
        unresolved_main_rvas.push_back(key.rva);
      }
    }
    out.emplace(key, std::move(rf));
  }

  const auto guesses = analyzeMainModuleSymbols(unresolved_main_rvas);
  for (const FrameKey &key : keys) {
    if (!have_executable_range ||
        !frameMatchesMainModule(key.raw_address, key.rva, executable_base,
                                executable_info.SizeOfImage)) {
      continue;
    }
    const auto guess = guesses.find(key.rva);
    if (guess == guesses.end()) {
      continue;
    }
    auto frame = out.find(key);
    if (frame != out.end()) {
      applySymbolGuessFallback(frame->second, key.rva, true, guess->second);
    }
  }
  return out;
}

bool isSleepFrame(std::uint64_t raw_address) {
  if (raw_address == 0) {
    return false;
  }

  DWORD64 module_base = SymGetModuleBase64(GetCurrentProcess(), raw_address);
  HMODULE module =
      reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(module_base));
  if (module == nullptr || (module != GetModuleHandleW(L"kernel32.dll") &&
                            module != GetModuleHandleW(L"KernelBase.dll") &&
                            module != GetModuleHandleW(L"ntdll.dll"))) {
    return false;
  }

  SymbolBuffer symbol{};
  symbol.info.SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol.info.MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  if (!SymFromAddr(GetCurrentProcess(), raw_address, &displacement,
                   &symbol.info)) {
    return false;
  }

  std::string_view name(symbol.info.Name, symbol.info.NameLen);
  for (std::string_view wait :
       {std::string_view("Sleep"), std::string_view("SleepEx"),
        std::string_view("WaitForSingleObject"),
        std::string_view("WaitForSingleObjectEx"),
        std::string_view("NtWaitForSingleObject"),
        std::string_view("ZwWaitForSingleObject"),
        std::string_view("NtDelayExecution"),
        std::string_view("ZwDelayExecution"),
        std::string_view("RtlDelayExecution")}) {
    if (name == wait) {
      return true;
    }
  }
  return false;
}

#endif

} // namespace spark
