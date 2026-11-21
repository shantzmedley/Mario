#include "EditorApp.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "mario/core/LevelIO.hpp"
#include "mario/editor/EditorPlayIntegration.hpp"
#include "mario/editor/INetworkPlaySession.hpp"

namespace {

float findLocalX(const mario::editor::EditorPlayIntegration& integration) {
    const auto playerId = integration.localPlayerId();
    const auto snapshot = integration.latestSnapshot();
    if (!playerId.has_value() || !snapshot.has_value()) {
        throw std::runtime_error("local player snapshot is unavailable");
    }
    for (const auto& entity : snapshot->entities) {
        if (entity.id == *playerId) {
            return entity.x;
        }
    }
    throw std::runtime_error("local player is missing from the snapshot");
}

int runNetworkSmoke(const std::filesystem::path& projectRoot) {
    try {
        const mario::core::ProjectPaths paths{projectRoot};
        const auto world = mario::core::LevelIO::loadFile(paths.levelFile(), paths);
        const auto authoringBefore = mario::core::LevelIO::saveString(world, paths);

        auto smokeWorld = world;
        mario::editor::EditorPlayIntegration integration{
            smokeWorld,
            mario::editor::createDefaultNetworkPlaySession()};
        std::string error;
        if (!integration.startLocalHost(error)) {
            throw std::runtime_error("Play failed to start: " + error);
        }

        const auto assignmentDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((!integration.localPlayerId() || !integration.latestSnapshot()) &&
               std::chrono::steady_clock::now() < assignmentDeadline) {
            integration.pump(1.0F / 60.0F);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const float initialX = findLocalX(integration);

        bool moved = false;
        const auto movementDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!moved && std::chrono::steady_clock::now() < movementDeadline) {
            mario::editor::ViewportInputState input;
            input.hovered = true;
            input.moveRight = true;
            integration.submitViewportInput(input);
            integration.pump(1.0F / 60.0F);
            moved = findLocalX(integration) > initialX;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!moved) {
            throw std::runtime_error("right input did not move the authoritative player");
        }

        mario::editor::ViewportInputState released;
        released.hovered = true;
        const float releaseX = findLocalX(integration);
        integration.submitViewportInput(released);
        for (int index = 0; index < 8; ++index) {
            integration.pump(1.0F / 60.0F);
        }
        if (std::abs(findLocalX(integration) - releaseX) >= 0.001F) {
            throw std::runtime_error(
                "authoritative player continued moving after input release");
        }
        integration.stop();
        integration.stop();

        const auto authoringAfter =
            mario::core::LevelIO::saveString(integration.authoringWorld(), paths);
        if (authoringAfter != authoringBefore) {
            throw std::runtime_error("runtime players leaked into authoring level data");
        }
        std::cout << "Mario network smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mario network smoke failed: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path projectRoot{MARIO_DEFAULT_PROJECT_ROOT};
    int frameLimit = 0;
    bool networkSmoke = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--project" && index + 1 < argc) {
            projectRoot = argv[++index];
        } else if (argument == "--smoke-test") {
            frameLimit = 3;
        } else if (argument == "--network-smoke-test") {
            networkSmoke = true;
        } else {
            std::cerr
                << "Usage: mario_editor [--project <repository-root>] "
                   "[--smoke-test] [--network-smoke-test]\n";
            return 2;
        }
    }

    if (networkSmoke) {
        return runNetworkSmoke(projectRoot);
    }

    mario::editor::EditorApp app{
        std::move(projectRoot),
        mario::editor::createDefaultNetworkPlaySession(),
        mario::editor::EditorRunOptions{
            frameLimit,
            frameLimit == 0,
            frameLimit == 0
        }};
    return app.run();
}
