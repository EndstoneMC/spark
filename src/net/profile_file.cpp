#include "net/profile_file.h"

#include <fstream>
#include <system_error>
#include <utility>

namespace spark {
namespace {

ProfileFileResult failure(std::string message)
{
    ProfileFileResult result;
    result.error = std::move(message);
    return result;
}

}  // namespace

std::filesystem::path profileStorageDirectory(const std::filesystem::path &data_folder)
{
    return data_folder / "profiles";
}

ProfileFileResult saveProfileToDirectory(const std::filesystem::path &folder,
                                         std::string_view profile_data,
                                         std::int64_t timestamp_ms)
{
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        return failure("could not create profile directory: " + error.message());
    }

    std::filesystem::path output;
    for (int suffix = 0; suffix < 1000; ++suffix) {
        std::string name = "profile-" + std::to_string(timestamp_ms);
        if (suffix != 0) {
            name += '-' + std::to_string(suffix);
        }
        name += ".sparkprofile";
        std::filesystem::path candidate = folder / name;
        if (!std::filesystem::exists(candidate, error)) {
            if (error) {
                return failure("could not inspect profile destination: " + error.message());
            }
            output = std::move(candidate);
            break;
        }
    }
    if (output.empty()) {
        return failure("could not choose a unique profile filename");
    }

    std::filesystem::path temporary = output;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(profile_data.data(),
                     static_cast<std::streamsize>(profile_data.size()));
        stream.close();
        if (!stream) {
            std::filesystem::remove(temporary, error);
            return failure("could not write temporary profile file");
        }
    }

    std::filesystem::rename(temporary, output, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return failure("could not finalize profile file: " + error.message());
    }

    ProfileFileResult result;
    result.ok = true;
    result.path = std::move(output);
    return result;
}

}  // namespace spark
