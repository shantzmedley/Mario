#include "mario/editor/INetworkPlaySession.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if MARIO_ENABLE_NETWORK_PLAY
#include <SyncVibe/Runtime.h>
#endif

namespace mario::editor {
namespace {

class UnavailableNetworkPlaySession final : public INetworkPlaySession {
public:
    bool available() const noexcept override {
        return false;
    }

    bool running() const noexcept override {
        return false;
    }

    std::string status() const override {
        return "SyncVibe runtime not linked";
    }

    PlaySessionState state() const override {
        PlaySessionState value;
        value.connection = PlayConnectionState::Unavailable;
        value.status = status();
        return value;
    }

    bool start(const PlaySessionConfig&, std::string& error) override {
        error = status();
        return false;
    }

    void pump(float) override {}
    void submitInput(PlayInput) override {}
    void stop() noexcept override {}

    std::optional<std::uint16_t> boundPort() const override {
        return std::nullopt;
    }

    std::optional<std::uint32_t> localPlayerId() const override {
        return std::nullopt;
    }

    std::optional<PlaySnapshot> latestSnapshot() const override {
        return std::nullopt;
    }

    std::vector<PlaySessionEvent> drainEvents() override {
        return {};
    }
};

#if MARIO_ENABLE_NETWORK_PLAY

using syncvibe::runtime::ClientSession;
using syncvibe::runtime::Event;
using syncvibe::runtime::EventType;
using syncvibe::runtime::ServerSession;
using syncvibe::runtime::Snapshot;
using syncvibe::runtime::Status;

PlaySnapshot convertSnapshot(const Snapshot& snapshot) {
    PlaySnapshot converted;
    converted.tick = snapshot.tick;
    converted.entities.reserve(snapshot.players.size());
    for (const auto& player : snapshot.players) {
        converted.entities.push_back({
            static_cast<std::uint32_t>(player.id),
            player.position.x,
            player.position.y
        });
    }
    return converted;
}

class SyncVibeNetworkPlaySession final : public INetworkPlaySession {
public:
    ~SyncVibeNetworkPlaySession() override {
        stop();
    }

    bool available() const noexcept override {
        return true;
    }

    bool running() const noexcept override {
        return running_;
    }

    std::string status() const override {
        return state_.status;
    }

    PlaySessionState state() const override {
        return state_;
    }

    bool start(const PlaySessionConfig& config, std::string& error) override {
        stop();
        events_.clear();
        desiredInput_ = {};
        runtimeEventError_.clear();
        state_ = {};
        state_.role = config.role;
        state_.connection = PlayConnectionState::Connecting;
        state_.endpoint = config.role == PlaySessionRole::Host
            ? "127.0.0.1"
            : config.host;
        state_.port = config.port;
        state_.status = config.role == PlaySessionRole::Host
            ? "Starting local host"
            : "Starting client";

        if (config.role == PlaySessionRole::Host) {
            server_ = std::make_unique<ServerSession>();
            const auto serverStatus = server_->Start(config.port);
            if (!serverStatus.ok()) {
                return failStart("Server start failed: " + serverStatus.message, error);
            }
            state_.port = server_->BoundPort();
            if (state_.port == 0) {
                return failStart("Server start failed: no bound port", error);
            }
        } else if (config.host.empty() || config.port == 0) {
            return failStart("Client start failed: host and port are required", error);
        }

        client_ = std::make_unique<ClientSession>();
        const auto clientStatus = client_->Start(state_.endpoint, state_.port);
        if (!clientStatus.ok()) {
            return failStart("Client start failed: " + clientStatus.message, error);
        }

        running_ = true;
        state_.status = config.role == PlaySessionRole::Host
            ? "Local host started; connecting client"
            : "Client started; connecting";
        events_.push_back({
            PlaySessionEventKind::Information,
            state_.status
        });
        drainRuntimeEvents();
        error.clear();
        return true;
    }

    void pump(const float deltaSeconds) override {
        if (!running_) {
            return;
        }
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F) {
            failRunning("Play pump failed: invalid delta time");
            return;
        }

        if (client_->LocalPlayerId().has_value()) {
            const auto inputStatus = client_->SubmitInput({
                desiredInput_.x,
                desiredInput_.y
            });
            if (!inputStatus.ok()) {
                failRunning("Input submission failed: " + inputStatus.message);
                return;
            }
        }

        auto status = client_->Pump(deltaSeconds);
        if (!status.ok()) {
            failRunning("Client pump failed: " + status.message);
            return;
        }
        if (server_) {
            status = server_->Pump(deltaSeconds);
            if (!status.ok()) {
                failRunning("Server pump failed: " + status.message);
                return;
            }
            status = client_->Pump(0.0F);
            if (!status.ok()) {
                failRunning("Client receive pump failed: " + status.message);
                return;
            }
        }

        drainRuntimeEvents();
        if (!runtimeEventError_.empty()) {
            failRunning(runtimeEventError_);
            runtimeEventError_.clear();
            return;
        }
        updateState();
    }

    void submitInput(const PlayInput input) override {
        if (!running_) {
            return;
        }
        const float length = std::hypot(input.x, input.y);
        if (!std::isfinite(length)) {
            failRunning("Input submission failed: direction must be finite");
            return;
        }
        if (length > 1.0F) {
            desiredInput_ = {input.x / length, input.y / length};
        } else {
            desiredInput_ = input;
        }
    }

    void stop() noexcept override {
        try {
            stopSessions();
            running_ = false;
            state_.connected = false;
            state_.assigned = false;
            state_.connection = available()
                ? PlayConnectionState::Stopped
                : PlayConnectionState::Unavailable;
            state_.status = "Stopped";
            state_.tick = 0;
            desiredInput_ = {};
        } catch (...) {
            running_ = false;
            state_.connection = PlayConnectionState::Error;
            state_.status = "Stop failed";
            state_.latestError = state_.status;
        }
    }

    std::optional<std::uint16_t> boundPort() const override {
        return state_.port == 0
            ? std::nullopt
            : std::optional<std::uint16_t>{state_.port};
    }

    std::optional<std::uint32_t> localPlayerId() const override {
        if (!client_) {
            return std::nullopt;
        }
        const auto id = client_->LocalPlayerId();
        return id.has_value()
            ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(*id)}
            : std::nullopt;
    }

    std::optional<PlaySnapshot> latestSnapshot() const override {
        if (server_ && server_->IsRunning()) {
            return convertSnapshot(server_->CurrentSnapshot());
        }
        if (client_ && client_->LatestSnapshot().has_value()) {
            return convertSnapshot(*client_->LatestSnapshot());
        }
        return std::nullopt;
    }

    std::vector<PlaySessionEvent> drainEvents() override {
        std::vector<PlaySessionEvent> result;
        result.swap(events_);
        return result;
    }

private:
    bool failStart(const std::string& message, std::string& error) {
        stopSessions();
        running_ = false;
        state_.connection = PlayConnectionState::Error;
        state_.status = message;
        state_.latestError = message;
        events_.push_back({PlaySessionEventKind::Error, message});
        error = message;
        return false;
    }

    void failRunning(const std::string& message) {
        stopSessions();
        running_ = false;
        state_.connected = false;
        state_.assigned = false;
        state_.connection = PlayConnectionState::Error;
        state_.status = message;
        state_.latestError = message;
        events_.push_back({PlaySessionEventKind::Error, message});
    }

    void stopSessions() noexcept {
        if (client_) {
            static_cast<void>(client_->Stop());
            client_.reset();
        }
        if (server_) {
            static_cast<void>(server_->Stop());
            server_.reset();
        }
    }

    void updateState() {
        state_.connected = client_ && client_->IsConnected();
        state_.assigned = localPlayerId().has_value();
        if (const auto snapshot = latestSnapshot()) {
            state_.tick = snapshot->tick;
        }

        if (state_.assigned) {
            state_.connection = PlayConnectionState::Assigned;
            state_.status = "Playing";
        } else if (state_.connected) {
            state_.connection = PlayConnectionState::Connected;
            state_.status = "Connected; awaiting player assignment";
        } else {
            state_.connection = PlayConnectionState::Connecting;
            state_.status = "Connecting";
        }
    }

    void drainRuntimeEvents() {
        if (server_) {
            for (const auto& event : server_->DrainEvents()) {
                translateEvent(event, true);
            }
        }
        if (client_) {
            for (const auto& event : client_->DrainEvents()) {
                translateEvent(event, false);
            }
        }
    }

    void translateEvent(const Event& event, const bool serverEvent) {
        PlaySessionEvent translated;
        switch (event.type) {
        case EventType::Connected:
            translated.kind = PlaySessionEventKind::Connected;
            translated.message = serverEvent
                ? "Local client connected to server"
                : "Connected to local server";
            break;
        case EventType::Disconnected:
            translated.kind = PlaySessionEventKind::Disconnected;
            translated.message = "Play-session peer disconnected";
            runtimeEventError_ = translated.message;
            break;
        case EventType::PlayerAssigned:
            translated.kind = PlaySessionEventKind::PlayerAssigned;
            translated.message =
                "Assigned player " + std::to_string(event.player_id);
            break;
        case EventType::ProtocolError:
        case EventType::TransportError:
            translated.kind = PlaySessionEventKind::Error;
            translated.message = event.message.empty()
                ? "SyncVibe runtime error"
                : event.message;
            state_.latestError = translated.message;
            runtimeEventError_ = translated.message;
            break;
        case EventType::SnapshotReceived:
            return;
        case EventType::Started:
        case EventType::Stopped:
            translated.kind = PlaySessionEventKind::Information;
            translated.message = serverEvent ? "Server started" : "Client started";
            break;
        }
        events_.push_back(std::move(translated));
    }

    PlaySessionState state_;
    PlayInput desiredInput_;
    std::string runtimeEventError_;
    bool running_ = false;
    std::unique_ptr<ServerSession> server_;
    std::unique_ptr<ClientSession> client_;
    std::vector<PlaySessionEvent> events_;
};

#endif

} // namespace

std::unique_ptr<INetworkPlaySession> createUnavailableNetworkPlaySession() {
    return std::make_unique<UnavailableNetworkPlaySession>();
}

std::unique_ptr<INetworkPlaySession> createDefaultNetworkPlaySession() {
#if MARIO_ENABLE_NETWORK_PLAY
    return std::make_unique<SyncVibeNetworkPlaySession>();
#else
    return createUnavailableNetworkPlaySession();
#endif
}

} // namespace mario::editor
