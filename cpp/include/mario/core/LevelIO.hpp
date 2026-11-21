#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include "mario/core/ProjectPaths.hpp"
#include "mario/core/World.hpp"

namespace mario::core {

class LevelFileOperations {
public:
    virtual ~LevelFileOperations() = default;

    [[nodiscard]] virtual std::filesystem::path temporaryPathFor(
        const std::filesystem::path& destination) = 0;
    virtual void writeDurably(
        const std::filesystem::path& temporaryFile,
        std::string_view contents) = 0;
    virtual void replace(
        const std::filesystem::path& temporaryFile,
        const std::filesystem::path& destination) = 0;
    virtual void removeTemporary(
        const std::filesystem::path& temporaryFile) noexcept = 0;
};

class LevelFormatError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LevelIO {
public:
    static constexpr int CurrentSchemaVersion = 1;

    [[nodiscard]] static World loadFile(
        const std::filesystem::path& file,
        const ProjectPaths& paths);
    [[nodiscard]] static World loadString(
        std::string_view json,
        const ProjectPaths& paths,
        std::string_view sourceName = "<memory>");

    static void saveFile(
        const std::filesystem::path& file,
        const World& world,
        const ProjectPaths& paths);
    static void saveFileWithOperations(
        const std::filesystem::path& file,
        const World& world,
        const ProjectPaths& paths,
        LevelFileOperations& operations);
    [[nodiscard]] static std::string saveString(
        const World& world,
        const ProjectPaths& paths);
};

} // namespace mario::core
