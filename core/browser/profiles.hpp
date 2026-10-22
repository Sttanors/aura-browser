#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace aura {
struct NamedProfile {
    int id = 0;
    std::string name;
    bool archived = false;
};
class ProfileCatalog {
  public:
    ProfileCatalog(std::filesystem::path current, std::string stored_root = {});
    std::filesystem::path root, current;
    std::vector<NamedProfile> items{{0, "Default profile", false}};
    std::string error;
    int current_id = 0;
    void reload();
    std::filesystem::path data_directory(int id) const;
    std::string name(int id, const std::string &language) const;
    int create(std::string name, const std::string &language);
    bool rename(int id, std::string name);
    bool archive(int id, bool archived);

  private:
    bool recovered_ = false;
    void save();
    bool valid_name(const std::string &name, int except = -1) const;
};
} // namespace aura
