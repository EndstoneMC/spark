#ifndef SPARK_APPLICATION_PROFILER_PROFILE_EXPORTER_H
#define SPARK_APPLICATION_PROFILER_PROFILE_EXPORTER_H

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "core/profiler/profiler.h"
#include "net/bytebin.h"
#include "net/cancellation.h"
#include "net/profile_file.h"

namespace spark {

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
struct ProfileExporterTestAccess;
struct ProfilerServiceTestAccess;
#endif

enum class ExportOutcome {
    Failed,
    Uploaded,
    Saved,
};

// Platform-independent profile exporter. Runs on a background thread.
// The ExportContext must be fully populated (including server and world
// metadata from the platform adapter) before calling exportProfile().
class ProfileExporter {
public:
    struct Result {
        ExportOutcome outcome = ExportOutcome::Failed;
        std::string message;
        // Shutdown cancellation keeps the recovery journal authoritative.
        bool retain_recovery_journal = false;
    };

    explicit ProfileExporter(std::filesystem::path storage_dir, std::string bytebin_url, std::string viewer_url);

    // Saves raw protobuf locally; bytebin uploads use gzip with local fallback.
    Result exportProfile(Profiler &profiler, const ExportContext &ctx, bool save_to_file,
                         const CancellationToken &cancellation = {});
    Result exportProfile(Profiler &profiler, ExportContext &&ctx, bool save_to_file,
                         const CancellationToken &cancellation = {});

private:
    Result exportProfile(Profiler &profiler, const ExportContext &ctx, bool save_to_file, ExportContext *owned_ctx,
                         const CancellationToken &cancellation);

#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    friend struct ProfileExporterTestAccess;
    friend struct ProfilerServiceTestAccess;

    using UploadFunction = std::function<UploadResult(const std::string &, const std::string &, const std::string &,
                                                      const std::string &, CancellationToken)>;
    using SaveFunction =
        std::function<ProfileFileResult(const std::filesystem::path &, std::string_view, std::int64_t)>;
#endif

    std::filesystem::path storage_dir_;
    std::string bytebin_url_;
    std::string viewer_url_;
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    UploadFunction upload_function_;
    SaveFunction save_function_;
#endif
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILE_EXPORTER_H
