#include "mario/core/LevelIO.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using mario::core::LevelFormatError;
using mario::core::LevelFileOperations;
using mario::core::LevelIO;
using mario::core::ProjectPaths;
using mario::core::UnknownComponent;
using mario::core::World;
using Json = nlohmann::json;

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

template <typename Function>
void requireFormatError(Function&& function, const std::string& expectedText) {
    try {
        function();
    } catch (const LevelFormatError& error) {
        require(
            std::string(error.what()).find(expectedText) != std::string::npos,
            "Format error did not mention '" + expectedText + "': " + error.what());
        return;
    }
    throw std::runtime_error("Expected LevelFormatError containing: " + expectedText);
}

void testCheckedInLevel(const ProjectPaths& paths) {
    const auto world = LevelIO::loadFile(paths.levelFile(), paths);
    require(!world.objects().empty(), "Checked-in level must contain at least one object");
    require(world.objects().front().id == 1, "First object ID was not preserved");

    mario::core::StableId maximumId = 0;
    for (const auto& object : world.objects()) {
        maximumId = std::max(maximumId, object.id);
    }
    require(
        world.nextObjectId() == maximumId + 1,
        "Next object ID must follow the maximum loaded ID");

    const auto* firstRenderer = world.objects().front().spriteRenderer();
    require(firstRenderer != nullptr, "First object must have a SpriteRenderer");
    require(firstRenderer->id == 2, "Component ID was not preserved");
    require(
        firstRenderer->sprite.texture.resolvedPath == paths.spriteSheet(),
        "Legacy absolute spritesheet path did not resolve to repository fallback");
    require(
        firstRenderer->sprite.texture.assetId == "spritesheet",
        "Legacy spritesheet did not acquire portable asset ID");
    require(
        paths.resolveTexture("", R"(Z:\another-machine\spritesheet.png)") ==
            paths.spriteSheet(),
        "Foreign Windows path did not use the cross-platform spritesheet fallback");
}

void testMalformedInput(const ProjectPaths& paths) {
    requireFormatError(
        [&] { static_cast<void>(LevelIO::loadString("{", paths, "invalid-json")); },
        "JSON");
    requireFormatError(
        [&] {
            static_cast<void>(LevelIO::loadString(
                R"([{"UniqueId":1,"name":"broken"}])", paths, "missing-fields"));
        },
        "zIndex");
    requireFormatError(
        [&] {
            static_cast<void>(LevelIO::loadString(
                R"({"schemaVersion":99,"objects":[]})", paths, "future-version"));
        },
        "unsupported version");
}

void testUnknownComponentPreservation(const ProjectPaths& paths) {
    const std::string level = R"([
      {
        "UniqueId": 41,
        "name": "Mystery",
        "components": [
          {
            "type": "third.party.CustomComponent",
            "properties": {
              "UniqueId": 91,
              "nested": {"message": "preserve me", "values": [1, 2, 3]}
            },
            "vendorMetadata": true
          }
        ],
        "transform": {
          "position": {"x": 32, "y": 64},
          "scale": {"x": 32, "y": 32}
        },
        "zIndex": 7
      }
    ])";
    const auto world = LevelIO::loadString(level, paths, "unknown-component");
    require(world.objects().size() == 1, "Unknown component test object not loaded");
    const auto* unknown = std::get_if<UnknownComponent>(&world.objects()[0].components[0]);
    require(unknown != nullptr, "Unknown component was not retained as opaque data");
    require(unknown->id == 91, "Unknown component ID was not retained");

    const auto saved = Json::parse(LevelIO::saveString(world, paths));
    const auto& payload = saved["objects"][0]["components"][0];
    require(
        payload["properties"]["nested"]["message"] == "preserve me",
        "Unknown component nested payload was changed");
    require(payload["vendorMetadata"] == true, "Unknown component metadata was lost");

    const auto reloaded = LevelIO::loadString(saved.dump(), paths, "unknown-reload");
    const auto* reloadedUnknown =
        std::get_if<UnknownComponent>(&reloaded.objects()[0].components[0]);
    require(reloadedUnknown != nullptr && reloadedUnknown->payload == payload,
        "Unknown component changed after save/reload");
}

void testRoundTrip(const ProjectPaths& paths) {
    const auto original = LevelIO::loadFile(paths.levelFile(), paths);
    const auto outputPath = uniqueTestPath("mario_core_roundtrip_level.json");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{outputPath};

    LevelIO::saveFile(outputPath, original, paths);
    const auto roundTrip = LevelIO::loadFile(outputPath, paths);
    require(roundTrip.objects().size() == original.objects().size(),
        "Round-trip object count changed");
    for (std::size_t index = 0; index < original.objects().size(); ++index) {
        const auto& before = original.objects()[index];
        const auto& after = roundTrip.objects()[index];
        require(after.id == before.id, "Round-trip object ID changed");
        require(after.components.size() == before.components.size(),
            "Round-trip component count changed");
        require(
            after.spriteRenderer()->id == before.spriteRenderer()->id,
            "Round-trip component ID changed");
        require(
            after.spriteRenderer()->sprite.texture.resolvedPath == paths.spriteSheet(),
            "Round-trip portable asset path did not resolve");
    }

    const auto text = LevelIO::saveString(original, paths);
    require(text.find("\"schemaVersion\": 1") != std::string::npos,
        "New writes must include schema version");
    require(
        text.find("C:\\\\Users\\\\Shant") == std::string::npos,
        "New writes must not retain legacy absolute texture paths");
    require(
        text.find("assets/images/spritesheet.png") != std::string::npos,
        "New writes must contain portable asset path");
}

class ForcedFailureOperations final : public LevelFileOperations {
public:
    enum class Stage {
        Write,
        Replace
    };

    ForcedFailureOperations(
        const Stage failureStage,
        std::filesystem::path temporaryFile)
        : failureStage_(failureStage),
          temporaryFile_(std::move(temporaryFile)) {}

    std::filesystem::path temporaryPathFor(
        const std::filesystem::path&) override {
        return temporaryFile_;
    }

    void writeDurably(
        const std::filesystem::path& temporaryFile,
        const std::string_view contents) override {
        std::ofstream output(temporaryFile, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Test could not create injected temporary file");
        }
        if (failureStage_ == Stage::Write) {
            output << "partial";
            output.close();
            throw std::runtime_error("Injected durable-write failure");
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        output.close();
        if (!output) {
            throw std::runtime_error("Test temporary write failed unexpectedly");
        }
    }

    void replace(
        const std::filesystem::path&,
        const std::filesystem::path&) override {
        throw std::runtime_error("Injected atomic-replace failure");
    }

    void removeTemporary(
        const std::filesystem::path& temporaryFile) noexcept override {
        std::error_code error;
        std::filesystem::remove(temporaryFile, error);
    }

private:
    Stage failureStage_;
    std::filesystem::path temporaryFile_;
};

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Unable to read test file: " + file.string());
    }
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

void testAtomicSave(const ProjectPaths& paths) {
    const auto testDirectory = uniqueTestPath("mario_atomic_save_tests");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{testDirectory};
    std::error_code cleanupError;
    std::filesystem::remove_all(testDirectory, cleanupError);
    std::filesystem::create_directories(testDirectory);

    const auto world = LevelIO::loadFile(paths.levelFile(), paths);
    const auto destination = testDirectory / "level.txt";
    const std::string priorValid = R"({"schemaVersion":1,"objects":[]})";
    {
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output << priorValid;
    }

    LevelIO::saveFile(destination, world, paths);
    require(
        LevelIO::loadFile(destination, paths).objects().size() ==
            world.objects().size(),
        "Atomic save did not replace the existing destination");
    require(
        readTextFile(destination) != priorValid,
        "Atomic save left the prior destination unchanged");

    const auto preserved = readTextFile(destination);
    for (const auto stage : {
             ForcedFailureOperations::Stage::Write,
             ForcedFailureOperations::Stage::Replace}) {
        const auto temporary = testDirectory /
            (stage == ForcedFailureOperations::Stage::Write
                ? "forced-write.tmp"
                : "forced-replace.tmp");
        ForcedFailureOperations operations{stage, temporary};
        bool failed = false;
        try {
            LevelIO::saveFileWithOperations(
                destination,
                world,
                paths,
                operations);
        } catch (const std::runtime_error& error) {
            failed = true;
            require(
                std::string(error.what()).find("Injected") != std::string::npos,
                "Atomic-save test caught an unexpected error");
        }
        require(failed, "Injected atomic-save failure did not fail");
        require(
            readTextFile(destination) == preserved,
            "Pre-replace failure corrupted the prior valid destination");
        require(
            !std::filesystem::exists(temporary),
            "Failed save left its specific temporary file behind");
    }
}

} // namespace

int main() {
    try {
        const ProjectPaths paths{std::filesystem::path{MARIO_SOURCE_DIR}};
        testCheckedInLevel(paths);
        testMalformedInput(paths);
        testUnknownComponentPreservation(paths);
        testRoundTrip(paths);
        testAtomicSave(paths);
        std::cout << "All Mario core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mario core tests failed: " << error.what() << '\n';
        return 1;
    }
}
