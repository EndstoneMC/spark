#include "application/profiler/profile_exporter.h"

#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "net/bytebin.h"
#include "net/gzip.h"
#include "net/profile_file.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ProfileExporter::ProfileExporter(std::filesystem::path storage_dir, std::string bytebin_url, std::string viewer_url)
    : storage_dir_(std::move(storage_dir)), bytebin_url_(std::move(bytebin_url)), viewer_url_(std::move(viewer_url))
{
}

ProfileExporter::Result ProfileExporter::exportProfile(Profiler &profiler, const ExportContext &ctx, bool save_to_file,
                                                       const CancellationToken &cancellation)
{
    return exportProfile(profiler, ctx, save_to_file, nullptr, cancellation);
}

ProfileExporter::Result ProfileExporter::exportProfile(Profiler &profiler, ExportContext &&ctx, bool save_to_file,
                                                       const CancellationToken &cancellation)
{
    return exportProfile(profiler, ctx, save_to_file, &ctx, cancellation);
}

ProfileExporter::Result ProfileExporter::exportProfile(Profiler &profiler, const ExportContext &ctx, bool save_to_file,
                                                       ExportContext *owned_ctx,
                                                       const CancellationToken &cancellation)
{
    Result result;
    const auto cancelled = [&]() {
        result.outcome = ExportOutcome::Failed;
        result.retain_recovery_journal = true;
        result.message = "Export cancelled during shutdown; recovery journal retained.";
        return result;
    };
    try {
        if (cancellation.stopRequested()) {
            return cancelled();
        }
        std::string body = owned_ctx != nullptr ? profiler.exportData(std::move(*owned_ctx)) : profiler.exportData(ctx);
        // Serialization has copied the completed allocation tree. Persistent
        // count-only may now safely reset/reuse the native allocation sampler
        // while gzip and network/file I/O continue in this export worker.
        std::string resume_error;
        profiler.resumePersistentAllocationCounting(resume_error);
        if (cancellation.stopRequested()) {
            return cancelled();
        }
        std::string compressed = gzipCompress(body, cancellation);
        if (cancellation.stopRequested()) {
            return cancelled();
        }
        if (save_to_file) {
            if (cancellation.stopRequested()) {
                return cancelled();
            }
            ProfileFileResult saved =
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
                save_function_ ? save_function_(storage_dir_, body, nowMs())
                               : saveProfileToDirectory(storage_dir_, body, nowMs());
#else
                saveProfileToDirectory(storage_dir_, body, nowMs());
#endif
            if (saved.ok) {
                result.outcome = ExportOutcome::Saved;
                result.message = "Saved to " + saved.path.string() + " - open it at " + viewer_url_;
                result.retain_recovery_journal = cancellation.stopRequested();
                if (result.retain_recovery_journal) {
                    result.message += " Shutdown requested; recovery journal retained.";
                }
            }
            else {
                result.message = "Failed to save the profile: " + saved.error;
            }
        }
        else {
            if (cancellation.stopRequested()) {
                return cancelled();
            }
            UploadResult upload_result =
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
                upload_function_ ? upload_function_(compressed, bytebin_url_, kSamplerContentType,
                                                    std::string("endstone-spark/") + kVersion, cancellation)
                                 : uploadToBytebin(compressed, bytebin_url_, kSamplerContentType,
                                                   std::string("endstone-spark/") + kVersion, cancellation);
#else
                uploadToBytebin(compressed, bytebin_url_, kSamplerContentType,
                                std::string("endstone-spark/") + kVersion, cancellation);
#endif
            if (upload_result.ok) {
                result.outcome = ExportOutcome::Uploaded;
                result.message = viewer_url_ + upload_result.key;
                result.retain_recovery_journal = cancellation.stopRequested();
                if (result.retain_recovery_journal) {
                    result.message += " Shutdown requested; recovery journal retained.";
                }
            }
            else {
                if (cancellation.stopRequested()) {
                    return cancelled();
                }
                ProfileFileResult saved =
#ifdef SPARK_ALLOCATION_LIFECYCLE_TESTING
                    save_function_ ? save_function_(storage_dir_, body, nowMs())
                                   : saveProfileToDirectory(storage_dir_, body, nowMs());
#else
                    saveProfileToDirectory(storage_dir_, body, nowMs());
#endif
                if (saved.ok) {
                    result.outcome = ExportOutcome::Saved;
                    result.message = "Upload failed (" + upload_result.error + "), so the profile was saved to " +
                                     saved.path.string() + " - open it at " + viewer_url_;
                    result.retain_recovery_journal = cancellation.stopRequested();
                    if (result.retain_recovery_journal) {
                        result.message += " Shutdown requested; recovery journal retained.";
                    }
                }
                else {
                    result.message = "Upload failed (" + upload_result.error + ") and automatic local save failed (" +
                                     saved.error + ").";
                }
            }
        }
    }
    catch (const std::exception &e) {
        std::string ignored;
        profiler.resumePersistentAllocationCounting(ignored);
        if (cancellation.stopRequested()) {
            return cancelled();
        }
        result.message = std::string("Export failed: ") + e.what();
    }
    catch (...) {
        std::string ignored;
        profiler.resumePersistentAllocationCounting(ignored);
        if (cancellation.stopRequested()) {
            return cancelled();
        }
        result.message = "Export failed with an unknown error.";
    }
    return result;
}

}  // namespace spark
