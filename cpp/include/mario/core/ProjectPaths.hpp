#pragma once

#include <filesystem>
#include <string>

namespace mario::core {

class ProjectPaths {
public:
    explicit ProjectPaths(std::filesystem::path repositoryRoot);

    [[nodiscard]] const std::filesystem::path& repositoryRoot() const noexcept;
    [[nodiscard]] const std::filesystem::path& assetsRoot() const noexcept;
    [[nodiscard]] const std::filesystem::path& levelFile() const noexcept;
    [[nodiscard]] const std::filesystem::path& spriteSheet() const noexcept;

    [[nodiscard]] std::filesystem::path resolveTexture(
        const std::string& assetId,
        const std::filesystem::path& serializedPath) const;
    [[nodiscard]] std::string portableTexturePath(
        const std::filesystem::path& resolvedPath) const;

private:
    std::filesystem::path repositoryRoot_;
    std::filesystem::path assetsRoot_;
    std::filesystem::path levelFile_;
    std::filesystem::path spriteSheet_;
};

} // namespace mario::core
