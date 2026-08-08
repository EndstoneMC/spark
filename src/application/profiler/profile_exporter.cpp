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
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ProfileExporter::ProfileExporter(std::filesystem::path storage_dir,
                                 std::string bytebin_url,
                                 std::string viewer_url)
    : storage_dir_(std::move(storage_dir)),
      bytebin_url_(std::move(bytebin_url)),
      viewer_url_(std::move(viewer_url))
{
}

ProfileExporter::Result ProfileExporter::exportProfile(const Profiler &profiler,
                                                        const ExportContext &ctx,
                                                        bool save_to_file)
{
    Result result;
    try {
        std::string body = profiler.exportData(ctx);
        std::string compressed = gzipCompress(body);
        if (save_to_file) {
            ProfileFileResult saved =
                saveProfileToDirectory(storage_dir_, compressed, nowMs());
            if (saved.ok) {
                result.outcome = ExportOutcome::Saved;
                result.message = "Saved to " + saved.path.string() +
                                 " - open it at " + viewer_url_;
            } else {
                result.message = "Failed to save the profile: " + saved.error;
            }
        } else {
            UploadResult upload_result =
                uploadToBytebin(compressed, bytebin_url_, kSamplerContentType,
                                std::string("endstone-spark/") + kVersion);
            if (upload_result.ok) {
                result.outcome = ExportOutcome::Uploaded;
                result.message = viewer_url_ + upload_result.key;
            } else {
                ProfileFileResult saved =
                    saveProfileToDirectory(storage_dir_, compressed, nowMs());
                if (saved.ok) {
                    result.outcome = ExportOutcome::Saved;
                    result.message = "Upload failed (" + upload_result.error +
                                     "), so the profile was saved to " +
                                     saved.path.string() +
                                     " - open it at " + viewer_url_;
                } else {
                    result.message = "Upload failed (" + upload_result.error +
                                     ") and automatic local save failed (" +
                                     saved.error + ").";
                }
            }
        }
    }
    catch (const std::exception &e) {
        result.message = std::string("Export failed: ") + e.what();
    }
    catch (...) {
        result.message = "Export failed with an unknown error.";
    }
    return result;
}

}  // namespace spark
