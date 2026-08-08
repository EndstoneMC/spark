#ifndef SPARK_APPLICATION_PROFILER_PROFILE_EXPORTER_H
#define SPARK_APPLICATION_PROFILER_PROFILE_EXPORTER_H

#include <filesystem>
#include <string>

#include "core/profiler/profiler.h"

namespace spark {

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
    };

    explicit ProfileExporter(std::filesystem::path storage_dir);

    // Performs protobuf export, gzip compression, and upload-or-save.
    // If save_to_file is true, saves locally; otherwise uploads to bytebin
    // and falls back to local save on upload failure.
    Result exportProfile(const Profiler &profiler, const ExportContext &ctx,
                         bool save_to_file);

private:
    std::filesystem::path storage_dir_;
};

}  // namespace spark

#endif  // SPARK_APPLICATION_PROFILER_PROFILE_EXPORTER_H
