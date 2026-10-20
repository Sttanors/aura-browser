#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace aura {
struct UpdateArtifact {
    std::string platform, kind, filename, sha256;
    long long size = 0;
};
struct UpdateRelease {
    std::string version, tag;
    long long sequence = 0, expires = 0;
    std::vector<UpdateArtifact> artifacts;
    std::string page_url() const;
    std::string download_url(const UpdateArtifact &artifact) const;
};
// Authenticate the exact bytes before parsing anything supplied by the network.
UpdateRelease verify_update_manifest(std::string_view bytes, std::string_view signature,
                                     std::string_view public_key, long long now);
void verify_update_artifact(const std::filesystem::path &path, const UpdateArtifact &artifact);
} // namespace aura
