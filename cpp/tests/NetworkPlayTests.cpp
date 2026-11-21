#include "mario/core/LevelIO.hpp"
#include "mario/editor/EditorPlayIntegration.hpp"
#include "mario/editor/INetworkPlaySession.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using mario::core::LevelIO;
using mario::core::ProjectPaths;
using mario::editor::EditorPlayIntegration;
using mario::editor::PlayConnectionState;
using mario::editor::ViewportInputState;

std::filesystem::path uniqueTestPath(const std::string& stem) {
    static std::atomic<std::uint64_t> counter{0};
    return std::filesystem::current_path() /
        (stem + "_" +
         std::to_string(
             std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
}

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Predicate>
void pumpUntil(
    EditorPlayIntegration& integration,
    Predicate&& predicate,
    const std::string& failure) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        integration.pump(1.0F / 60.0F);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(predicate(), failure + ": " + integration.status());
}

float localPlayerX(const EditorPlayIntegration& integration) {
    const auto playerId = integration.localPlayerId();
    const auto snapshot = integration.latestSnapshot();
    require(playerId.has_value(), "Local player ID is unavailable");
    require(snapshot.has_value(), "Play snapshot is unavailable");
    for (const auto& entity : snapshot->entities) {
        if (entity.id == *playerId) {
            return entity.x;
        }
    }
    throw std::runtime_error("Local player is missing from snapshot");
}

void testLoopbackAndIsolation(const ProjectPaths& paths) {
    auto world = LevelIO::loadFile(paths.levelFile(), paths);
    const auto authoringBefore = LevelIO::saveString(world, paths);
    const auto objectCount = world.objects().size();

    EditorPlayIntegration integration{
        world,
        mario::editor::createDefaultNetworkPlaySession()};
    require(integration.available(), "SyncVibe adapter is unavailable");
    std::string error;
    require(integration.startLocalHost(error), "Local host failed to start: " + error);
    require(integration.running(), "Integration is not running after successful start");
    require(integration.boundPort().value_or(0) != 0, "Local host has no bound port");

    pumpUntil(
        integration,
        [&] {
            return integration.localPlayerId().has_value() &&
                integration.latestSnapshot().has_value();
        },
        "Loopback client never reached assigned state");
    require(
        integration.state().connection == PlayConnectionState::Assigned,
        "Integration did not report assigned/playing state");

    const auto proxies = integration.runtimePlayers();
    require(!proxies.empty(), "Presentation path did not produce runtime proxies");
    require(
        std::any_of(
            proxies.begin(),
            proxies.end(),
            [](const auto& proxy) { return proxy.local; }),
        "Presentation path did not identify the local player");
    require(
        integration.authoringWorld().objects().size() == objectCount,
        "Presentation proxies entered the authoring world");

    const float initialX = localPlayerX(integration);
    ViewportInputState rightInput;
    rightInput.hovered = true;
    rightInput.moveRight = true;
    integration.submitViewportInput(rightInput);
    pumpUntil(
        integration,
        [&] { return localPlayerX(integration) > initialX; },
        "Rightward input did not increase authoritative X");
    const float releaseX = localPlayerX(integration);

    ViewportInputState released;
    released.focused = true;
    integration.submitViewportInput(released);
    for (int index = 0; index < 12; ++index) {
        integration.pump(1.0F / 60.0F);
    }
    require(
        std::abs(localPlayerX(integration) - releaseX) < 0.001F,
        "Authoritative X continued moving after zero input release");

    const auto stopStart = std::chrono::steady_clock::now();
    integration.stop();
    integration.stop();
    const auto stopDuration = std::chrono::steady_clock::now() - stopStart;
    require(stopDuration < std::chrono::seconds(1), "Stop was not bounded");
    require(!integration.running(), "Integration remained running after Stop");
    require(
        integration.runtimePlayers().empty(),
        "Stopped integration retained runtime presentation proxies");

    require(world.objects().size() == objectCount, "Runtime changed authoring object count");
    require(
        LevelIO::saveString(world, paths) == authoringBefore,
        "Runtime changed authoring world data");

    const auto roundTripPath =
        uniqueTestPath("mario_network_authoring_roundtrip.json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code errorCode;
            std::filesystem::remove(path, errorCode);
        }
    } cleanup{roundTripPath};
    LevelIO::saveFile(roundTripPath, world, paths);
    const auto reloaded = LevelIO::loadFile(roundTripPath, paths);
    require(
        reloaded.objects().size() == objectCount,
        "Runtime proxies entered saved authoring object data");
    require(
        LevelIO::saveString(reloaded, paths) == authoringBefore,
        "Stop/save round trip changed authoring data");
}

void testInputPolicy() {
    ViewportInputState diagonal;
    diagonal.hovered = true;
    diagonal.moveRight = true;
    diagonal.moveUp = true;
    const auto normalized = mario::editor::normalizedViewportInput(diagonal, true);
    require(
        std::abs(std::hypot(normalized.x, normalized.y) - 1.0F) < 0.0001F,
        "Diagonal viewport input was not normalized");
    require(normalized.x > 0.0F && normalized.y > 0.0F,
        "Diagonal viewport input has the wrong direction");

    ViewportInputState inactive = diagonal;
    inactive.hovered = false;
    inactive.focused = false;
    const auto suppressed = mario::editor::normalizedViewportInput(inactive, true);
    require(
        suppressed.x == 0.0F && suppressed.y == 0.0F,
        "Inactive viewport leaked gameplay input");

    diagonal.hovered = false;
    diagonal.focused = true;
    const auto focused = mario::editor::normalizedViewportInput(diagonal, true);
    require(focused.x > 0.0F && focused.y > 0.0F,
        "Focused viewport did not accept gameplay input");

    const auto stopped = mario::editor::normalizedViewportInput(diagonal, false);
    require(stopped.x == 0.0F && stopped.y == 0.0F,
        "Stopped Play mode accepted gameplay input");
}

void testFailuresSurfaceErrors() {
    mario::core::World unavailableWorld;
    EditorPlayIntegration unavailable{
        unavailableWorld,
        mario::editor::createUnavailableNetworkPlaySession()};
    std::string error;
    require(!unavailable.startLocalHost(error), "Unavailable adapter unexpectedly started");
    require(!error.empty(), "Unavailable adapter did not surface an error");

    mario::core::World invalidWorld;
    EditorPlayIntegration invalid{
        invalidWorld,
        mario::editor::createDefaultNetworkPlaySession()};
    error.clear();
    require(
        !invalid.startClient("", 0, error),
        "Invalid remote client configuration unexpectedly started");
    require(!invalid.running(), "Failed start reported a running session");
    require(!error.empty(), "Failed start did not surface an error");
    require(
        invalid.state().connection == PlayConnectionState::Error,
        "Failed start did not retain error state");
}

} // namespace

int main() {
    try {
        const ProjectPaths paths{std::filesystem::path{MARIO_SOURCE_DIR}};
        testLoopbackAndIsolation(paths);
        testInputPolicy();
        testFailuresSurfaceErrors();
        std::cout << "All Mario network play tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mario network play tests failed: " << error.what() << '\n';
        return 1;
    }
}
