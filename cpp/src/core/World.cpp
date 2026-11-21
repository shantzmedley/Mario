#include "mario/core/World.hpp"

#include <algorithm>
#include <stdexcept>

namespace mario::core {

SpriteRenderer* GameObject::spriteRenderer() {
    for (auto& component : components) {
        if (auto* sprite = std::get_if<SpriteRenderer>(&component)) {
            return sprite;
        }
    }
    return nullptr;
}

const SpriteRenderer* GameObject::spriteRenderer() const {
    for (const auto& component : components) {
        if (const auto* sprite = std::get_if<SpriteRenderer>(&component)) {
            return sprite;
        }
    }
    return nullptr;
}

const std::vector<GameObject>& World::objects() const noexcept {
    return objects_;
}

std::vector<GameObject>& World::objects() noexcept {
    return objects_;
}

GameObject* World::find(const StableId id) {
    const auto found = std::find_if(
        objects_.begin(), objects_.end(),
        [id](const GameObject& object) { return object.id == id; });
    return found == objects_.end() ? nullptr : &*found;
}

const GameObject* World::find(const StableId id) const {
    const auto found = std::find_if(
        objects_.begin(), objects_.end(),
        [id](const GameObject& object) { return object.id == id; });
    return found == objects_.end() ? nullptr : &*found;
}

GameObject& World::createSpriteObject(
    const std::size_t tileIndex,
    const Vec2 position,
    const std::filesystem::path& spriteSheetPath) {
    if (tileIndex >= 26) {
        throw std::out_of_range("Sprite tile index must be in [0, 25]");
    }

    GameObject object;
    object.id = nextObjectId_++;
    object.name = "Sprite_Object_Gen";
    object.transform.position = position;
    object.transform.scale = {32.0F, 32.0F};

    SpriteRenderer renderer;
    renderer.id = nextComponentId_++;
    renderer.sprite.texture.resolvedPath = spriteSheetPath;
    renderer.sprite.textureCoordinates = spriteCoordinatesForTile(tileIndex);
    object.components.emplace_back(std::move(renderer));

    objects_.emplace_back(std::move(object));
    return objects_.back();
}

void World::replaceObjects(std::vector<GameObject> objects) {
    objects_ = std::move(objects);
    recalculateNextIds();
}

void World::recalculateNextIds() {
    StableId maxObject = 0;
    StableId maxComponent = 0;
    for (const auto& object : objects_) {
        maxObject = std::max(maxObject, object.id);
        for (const auto& component : object.components) {
            std::visit(
                [&maxComponent](const auto& value) {
                    maxComponent = std::max(maxComponent, value.id);
                },
                component);
        }
    }
    nextObjectId_ = maxObject + 1;
    nextComponentId_ = maxComponent + 1;
}

StableId World::nextObjectId() const noexcept {
    return nextObjectId_;
}

StableId World::nextComponentId() const noexcept {
    return nextComponentId_;
}

std::array<Vec2, 4> spriteCoordinatesForTile(
    const std::size_t tileIndex,
    const int sheetWidth,
    const int sheetHeight,
    const int tileWidth,
    const int tileHeight) {
    if (sheetWidth <= 0 || sheetHeight <= 0 || tileWidth <= 0 || tileHeight <= 0) {
        throw std::invalid_argument("Sprite sheet and tile dimensions must be positive");
    }
    const auto columns = static_cast<std::size_t>(sheetWidth / tileWidth);
    const auto rows = static_cast<std::size_t>(sheetHeight / tileHeight);
    if (columns == 0 || rows == 0 || tileIndex >= columns * rows) {
        throw std::out_of_range("Sprite tile index exceeds sprite sheet dimensions");
    }

    const auto column = tileIndex % columns;
    const auto rowFromTop = tileIndex / columns;
    const float left = static_cast<float>(column * tileWidth) / static_cast<float>(sheetWidth);
    const float right = static_cast<float>((column + 1) * tileWidth) / static_cast<float>(sheetWidth);
    const float top = 1.0F - static_cast<float>(rowFromTop * tileHeight) / static_cast<float>(sheetHeight);
    const float bottom =
        1.0F - static_cast<float>((rowFromTop + 1) * tileHeight) / static_cast<float>(sheetHeight);
    return {{{right, top}, {right, bottom}, {left, bottom}, {left, top}}};
}

} // namespace mario::core
