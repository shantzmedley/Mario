#include "mario/editor/EditorPlayIntegration.hpp"

#include <cmath>
#include <utility>

namespace mario::editor {

PlayInput normalizedViewportInput(
    const ViewportInputState& input,
    const bool playing) {
    PlayInput direction;
    if (!playing || (!input.hovered && !input.focused)) {
        return direction;
    }

    direction.x =
        (input.moveRight ? 1.0F : 0.0F) -
        (input.moveLeft ? 1.0F : 0.0F);
    direction.y =
        (input.moveUp ? 1.0F : 0.0F) -
        (input.moveDown ? 1.0F : 0.0F);
    const float length = std::hypot(direction.x, direction.y);
    if (length > 1.0F) {
        direction.x /= length;
        direction.y /= length;
    }
    return direction;
}

EditorPlayIntegration::EditorPlayIntegration(
    core::World& authoringWorld,
    std::unique_ptr<INetworkPlaySession> session)
    : authoringWorld_(authoringWorld),
      controller_(
          session ? std::move(session) : createUnavailableNetworkPlaySession()) {}

bool EditorPlayIntegration::available() const noexcept {
    return controller_.available();
}

bool EditorPlayIntegration::running() const noexcept {
    return controller_.running();
}

std::string EditorPlayIntegration::status() const {
    return controller_.status();
}

PlaySessionState EditorPlayIntegration::state() const {
    return controller_.state();
}

bool EditorPlayIntegration::startLocalHost(
    std::string& error,
    const std::uint16_t port) {
    return controller_.startHost(port, error);
}

bool EditorPlayIntegration::startClient(
    std::string host,
    const std::uint16_t port,
    std::string& error) {
    return controller_.startClient(std::move(host), port, error);
}

void EditorPlayIntegration::pump(const float deltaSeconds) {
    controller_.pump(deltaSeconds);
}

void EditorPlayIntegration::stop() noexcept {
    controller_.stop();
}

void EditorPlayIntegration::submitViewportInput(const ViewportInputState& input) {
    controller_.submitInput(normalizedViewportInput(input, running()));
}

std::optional<std::uint16_t> EditorPlayIntegration::boundPort() const {
    return controller_.boundPort();
}

std::optional<std::uint32_t> EditorPlayIntegration::localPlayerId() const {
    return controller_.localPlayerId();
}

std::optional<PlaySnapshot> EditorPlayIntegration::latestSnapshot() const {
    return controller_.latestSnapshot();
}

std::vector<PlaySessionEvent> EditorPlayIntegration::drainEvents() {
    return controller_.drainEvents();
}

std::vector<RuntimePlayerProxy> EditorPlayIntegration::runtimePlayers() const {
    std::vector<RuntimePlayerProxy> proxies;
    const auto snapshot = controller_.latestSnapshot();
    if (!snapshot.has_value()) {
        return proxies;
    }

    const auto localPlayer = controller_.localPlayerId();
    proxies.reserve(snapshot->entities.size());
    for (const auto& entity : snapshot->entities) {
        const bool local = localPlayer.has_value() && entity.id == *localPlayer;
        proxies.push_back({
            entity.id,
            entity.x,
            entity.y,
            32.0F,
            local,
            local
                ? static_cast<std::uint8_t>(65)
                : static_cast<std::uint8_t>(150 + (entity.id * 37U) % 90U),
            local
                ? static_cast<std::uint8_t>(225)
                : static_cast<std::uint8_t>(70 + (entity.id * 53U) % 100U),
            local
                ? static_cast<std::uint8_t>(95)
                : static_cast<std::uint8_t>(155 + (entity.id * 29U) % 90U)
        });
    }
    return proxies;
}

core::World& EditorPlayIntegration::authoringWorld() noexcept {
    return authoringWorld_;
}

const core::World& EditorPlayIntegration::authoringWorld() const noexcept {
    return authoringWorld_;
}

} // namespace mario::editor
