#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mario::editor {

enum class PlaySessionRole {
    Host,
    Client
};

struct PlaySessionConfig {
    PlaySessionRole role = PlaySessionRole::Host;
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
};

struct PlayInput {
    float x = 0.0F;
    float y = 0.0F;
};

struct PlayEntityState {
    std::uint32_t id = 0;
    float x = 0.0F;
    float y = 0.0F;
};

struct PlaySnapshot {
    std::uint64_t tick = 0;
    std::vector<PlayEntityState> entities;
};

enum class PlaySessionEventKind {
    Information,
    Connected,
    Disconnected,
    PlayerAssigned,
    Error
};

struct PlaySessionEvent {
    PlaySessionEventKind kind = PlaySessionEventKind::Information;
    std::string message;
};

enum class PlayConnectionState {
    Unavailable,
    Stopped,
    Connecting,
    Connected,
    Assigned,
    Error
};

struct PlaySessionState {
    PlaySessionRole role = PlaySessionRole::Host;
    PlayConnectionState connection = PlayConnectionState::Stopped;
    std::string endpoint;
    std::uint16_t port = 0;
    bool connected = false;
    bool assigned = false;
    std::uint64_t tick = 0;
    std::string status = "Stopped";
    std::string latestError;
};

class INetworkPlaySession {
public:
    virtual ~INetworkPlaySession() = default;

    [[nodiscard]] virtual bool available() const noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] virtual std::string status() const = 0;
    [[nodiscard]] virtual PlaySessionState state() const = 0;

    virtual bool start(const PlaySessionConfig& config, std::string& error) = 0;
    virtual void pump(float deltaSeconds) = 0;
    virtual void submitInput(PlayInput input) = 0;
    virtual void stop() noexcept = 0;

    [[nodiscard]] virtual std::optional<std::uint16_t> boundPort() const = 0;
    [[nodiscard]] virtual std::optional<std::uint32_t> localPlayerId() const = 0;
    [[nodiscard]] virtual std::optional<PlaySnapshot> latestSnapshot() const = 0;
    [[nodiscard]] virtual std::vector<PlaySessionEvent> drainEvents() = 0;
};

class PlaySessionController {
public:
    explicit PlaySessionController(std::unique_ptr<INetworkPlaySession> session)
        : session_(std::move(session)) {}

    [[nodiscard]] bool available() const noexcept {
        return session_ != nullptr && session_->available();
    }

    [[nodiscard]] bool running() const noexcept {
        return session_ != nullptr && session_->running();
    }

    [[nodiscard]] std::string status() const {
        return session_ == nullptr ? "No play-session adapter installed" : session_->status();
    }

    [[nodiscard]] PlaySessionState state() const {
        if (session_ == nullptr) {
            PlaySessionState value;
            value.connection = PlayConnectionState::Unavailable;
            value.status = "No play-session adapter installed";
            return value;
        }
        return session_->state();
    }

    bool startHost(const std::uint16_t port, std::string& error) {
        return session_ != nullptr &&
            session_->start(PlaySessionConfig{PlaySessionRole::Host, {}, port}, error);
    }

    bool startClient(
        std::string host,
        const std::uint16_t port,
        std::string& error) {
        return session_ != nullptr &&
            session_->start(
                PlaySessionConfig{PlaySessionRole::Client, std::move(host), port},
                error);
    }

    void pump(const float deltaSeconds) {
        if (running()) {
            session_->pump(deltaSeconds);
        }
    }

    void submitInput(const PlayInput input) {
        if (running()) {
            session_->submitInput(input);
        }
    }

    void stop() noexcept {
        if (session_ != nullptr) {
            session_->stop();
        }
    }

    [[nodiscard]] std::optional<std::uint16_t> boundPort() const {
        return session_ == nullptr ? std::nullopt : session_->boundPort();
    }

    [[nodiscard]] std::optional<std::uint32_t> localPlayerId() const {
        return session_ == nullptr ? std::nullopt : session_->localPlayerId();
    }

    [[nodiscard]] std::optional<PlaySnapshot> latestSnapshot() const {
        return session_ == nullptr ? std::nullopt : session_->latestSnapshot();
    }

    [[nodiscard]] std::vector<PlaySessionEvent> drainEvents() {
        return session_ == nullptr
            ? std::vector<PlaySessionEvent>{}
            : session_->drainEvents();
    }

private:
    std::unique_ptr<INetworkPlaySession> session_;
};

[[nodiscard]] std::unique_ptr<INetworkPlaySession> createDefaultNetworkPlaySession();
[[nodiscard]] std::unique_ptr<INetworkPlaySession> createUnavailableNetworkPlaySession();

} // namespace mario::editor
