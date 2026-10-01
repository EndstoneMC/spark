#ifndef SPARK_CORE_RECOVERY_REPLAY_GUARD_H
#define SPARK_CORE_RECOVERY_REPLAY_GUARD_H

#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace spark {

inline std::filesystem::path recoveryReplayGuardPath(const std::filesystem::path &recovery_directory)
{
    return recovery_directory.parent_path() / (recovery_directory.filename().string() + ".replay-blocked");
}

inline bool isRecoveryArtifact(std::string_view name) noexcept
{
    if (name == "metadata.snapshot" || name == "metadata.snapshot.tmp") {
        return true;
    }
    if (!name.starts_with("segment-")) {
        return false;
    }
    return name.ends_with(".jnl") || name.ends_with(".jnl.tmp");
}

namespace detail {

inline void captureReplayGuardException(std::error_code &error) noexcept
{
    try {
        throw;
    }
    catch (const std::filesystem::filesystem_error &exception) {
        error = exception.code();
    }
    catch (const std::system_error &exception) {
        error = exception.code();
    }
    catch (...) {
        error = std::make_error_code(std::errc::io_error);
    }
}

inline bool markerMissingError(const std::error_code &error) noexcept
{
    return error == std::errc::no_such_file_or_directory;
}

}  // namespace detail

inline bool recoveryReplayBlocked(const std::filesystem::path &recovery_directory, std::error_code &error) noexcept
{
    error.clear();
    try {
        const auto marker = recoveryReplayGuardPath(recovery_directory);
        const auto status = std::filesystem::symlink_status(marker, error);
        if (error) {
            if (detail::markerMissingError(error)) {
                error.clear();
                return false;
            }
            return true;
        }
        return status.type() != std::filesystem::file_type::not_found;
    }
    catch (...) {
        detail::captureReplayGuardException(error);
        return true;
    }
}

inline bool blockRecoveryReplay(const std::filesystem::path &recovery_directory, std::error_code &error) noexcept
{
    error.clear();
    try {
        const auto marker = recoveryReplayGuardPath(recovery_directory);
        const auto status = std::filesystem::symlink_status(marker, error);
        if (!error && status.type() != std::filesystem::file_type::not_found) {
            return true;
        }
        if (error && !detail::markerMissingError(error)) {
            return false;
        }
        error.clear();

        std::ofstream output(marker, std::ios::binary | std::ios::out | std::ios::app);
        if (!output) {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        output.flush();
        if (!output) {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        output.close();
        if (!output) {
            error = std::make_error_code(std::errc::io_error);
            return false;
        }
        return true;
    }
    catch (...) {
        detail::captureReplayGuardException(error);
        return false;
    }
}

inline bool clearRecoveryReplayBlock(const std::filesystem::path &recovery_directory, std::error_code &error) noexcept
{
    error.clear();
    try {
        std::filesystem::remove(recoveryReplayGuardPath(recovery_directory), error);
        return !error;
    }
    catch (...) {
        detail::captureReplayGuardException(error);
        return false;
    }
}

}  // namespace spark

#endif  // SPARK_CORE_RECOVERY_REPLAY_GUARD_H
