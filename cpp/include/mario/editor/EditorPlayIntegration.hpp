#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mario/core/World.hpp"
#include "mario/editor/INetworkPlaySession.hpp"

namespace mario::editor {

struct ViewportInputState {
    bool hovered = false;
    bool focused = false;
    bool moveLeft = false;
    bool moveRight = false;
    bool moveUp = false;
    bool moveDown = false;
};

struct RuntimePlayerProxy {
    std::uint32_t id = 0;
    float x = 0.0F;
    float y = 0.0F;
    float size = 32.0F;
    bool local = false;
    std::uint8_t red = 255;
    std::uint8_t green = 255;
    std::uint8_t blue = 255;
};

[[nodiscard]] PlayInput normalizedViewportInput(
    const ViewportInputState& input,
    bool playing);

class EditorPlayIntegration {
public:
    EditorPlayIntegration(
        core::World& authoringWorld,
        std::unique_ptr<INetworkPlaySession> session);

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string status() const;
    [[nodiscard]] PlaySessionState state() const;

    bool startLocalHost(std::string& error, std::uint16_t port = 0);
    bool startClient(std::string host, std::uint16_t port, std::string& error);
    void pump(float deltaSeconds);
    void stop() noexcept;
    void submitViewportInput(const ViewportInputState& input);

    [[nodiscard]] std::optional<std::uint16_t> boundPort() const;
    [[nodiscard]] std::optional<std::uint32_t> localPlayerId() const;
    [[nodiscard]] std::optional<PlaySnapshot> latestSnapshot() const;
    [[nodiscard]] std::vector<PlaySessionEvent> drainEvents();
    [[nodiscard]] std::vector<RuntimePlayerProxy> runtimePlayers() const;

    [[nodiscard]] core::World& authoringWorld() noexcept;
    [[nodiscard]] const core::World& authoringWorld() const noexcept;

private:
    core::World& authoringWorld_;
    PlaySessionController controller_;
};

} // namespace mario::editor
