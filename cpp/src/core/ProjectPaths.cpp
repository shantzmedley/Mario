#include "mario/core/ProjectPaths.hpp"

#include <system_error>

namespace mario::core {
namespace {

std::filesystem::path normalizedAbsolute(std::filesystem::path path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(std::move(path), error);
    if (error) {
        return path.lexically_normal();
    }
    return absolute.lexically_normal();
}

bool isRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

std::string portableFilename(const std::filesystem::path& path) {
    const auto text = path.generic_string();
    const auto separator = text.find_last_of("/\\");
    return separator == std::string::npos ? text : text.substr(separator + 1);
}

} // namespace

ProjectPaths::ProjectPaths(std::filesystem::path repositoryRoot)
    : repositoryRoot_(normalizedAbsolute(std::move(repositoryRoot))),
      assetsRoot_(repositoryRoot_ / "assets"),
      levelFile_(repositoryRoot_ / "level.txt"),
      spriteSheet_(assetsRoot_ / "images" / "spritesheet.png") {}

const std::filesystem::path& ProjectPaths::repositoryRoot() const noexcept {
    return repositoryRoot_;
}

const std::filesystem::path& ProjectPaths::assetsRoot() const noexcept {
    return assetsRoot_;
}

const std::filesystem::path& ProjectPaths::levelFile() const noexcept {
    return levelFile_;
}

const std::filesystem::path& ProjectPaths::spriteSheet() const noexcept {
    return spriteSheet_;
}

std::filesystem::path ProjectPaths::resolveTexture(
    const std::string& assetId,
    const std::filesystem::path& serializedPath) const {
    if (assetId == "spritesheet" && isRegularFile(spriteSheet_)) {
        return spriteSheet_;
    }

    if (!serializedPath.empty()) {
        const auto candidate = serializedPath.is_absolute()
            ? serializedPath
            : repositoryRoot_ / serializedPath;
        if (isRegularFile(candidate)) {
            return normalizedAbsolute(candidate);
        }
        if (portableFilename(serializedPath) == spriteSheet_.filename().string() &&
            isRegularFile(spriteSheet_)) {
            return spriteSheet_;
        }
    }

    return serializedPath.is_absolute()
        ? serializedPath.lexically_normal()
        : (repositoryRoot_ / serializedPath).lexically_normal();
}

std::string ProjectPaths::portableTexturePath(const std::filesystem::path& resolvedPath) const {
    std::error_code error;
    auto relative = std::filesystem::relative(resolvedPath, repositoryRoot_, error);
    if (!error && !relative.empty()) {
        const auto text = relative.generic_string();
        if (!text.starts_with("../") && text != "..") {
            return text;
        }
    }
    if (portableFilename(resolvedPath) == spriteSheet_.filename().string()) {
        return "assets/images/spritesheet.png";
    }
    return resolvedPath.generic_string();
}

} // namespace mario::core
