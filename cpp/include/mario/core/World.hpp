#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace mario::core {

using StableId = std::uint64_t;

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
};

struct Color {
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;
};

struct Transform {
    Vec2 position{};
    Vec2 scale{32.0F, 32.0F};
};

struct TextureReference {
    std::string assetId = "spritesheet";
    std::filesystem::path resolvedPath;
    int pixelWidth = 224;
    int pixelHeight = 32;
};

struct Sprite {
    float width = 16.0F;
    float height = 16.0F;
    TextureReference texture{};
    std::array<Vec2, 4> textureCoordinates{{
        {1.0F, 1.0F}, {1.0F, 0.0F}, {0.0F, 0.0F}, {0.0F, 1.0F}
    }};
};

struct SpriteRenderer {
    StableId id = 0;
    Sprite sprite{};
    Color tint{};
    nlohmann::json preservedComponent = nlohmann::json::object();
};

struct UnknownComponent {
    StableId id = 0;
    std::string type;
    nlohmann::json payload = nlohmann::json::object();
};

using Component = std::variant<SpriteRenderer, UnknownComponent>;

struct GameObject {
    StableId id = 0;
    std::string name;
    Transform transform{};
    int zIndex = 0;
    std::vector<Component> components;
    nlohmann::json preservedObject = nlohmann::json::object();

    [[nodiscard]] SpriteRenderer* spriteRenderer();
    [[nodiscard]] const SpriteRenderer* spriteRenderer() const;
};

class World {
public:
    [[nodiscard]] const std::vector<GameObject>& objects() const noexcept;
    [[nodiscard]] std::vector<GameObject>& objects() noexcept;
    [[nodiscard]] GameObject* find(StableId id);
    [[nodiscard]] const GameObject* find(StableId id) const;

    GameObject& createSpriteObject(
        std::size_t tileIndex,
        Vec2 position,
        const std::filesystem::path& spriteSheetPath);

    void replaceObjects(std::vector<GameObject> objects);
    void recalculateNextIds();

    [[nodiscard]] StableId nextObjectId() const noexcept;
    [[nodiscard]] StableId nextComponentId() const noexcept;

    nlohmann::json documentMetadata = nlohmann::json::object();

private:
    std::vector<GameObject> objects_;
    StableId nextObjectId_ = 1;
    StableId nextComponentId_ = 1;
};

[[nodiscard]] std::array<Vec2, 4> spriteCoordinatesForTile(
    std::size_t tileIndex,
    int sheetWidth = 224,
    int sheetHeight = 32,
    int tileWidth = 16,
    int tileHeight = 16);

} // namespace mario::core
