#include "mario/core/LevelIO.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mario::core {
namespace {

using Json = nlohmann::json;

constexpr std::string_view SpriteRendererType = "tsea.components.SpriteRenderer";
std::atomic<std::uint64_t> TemporaryFileCounter{0};

std::string portableFilename(const std::string& path) {
    const auto separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

[[noreturn]] void fail(
    const std::string_view source,
    const std::string& location,
    const std::string& message) {
    throw LevelFormatError(
        std::string(source) + ": " + location + ": " + message);
}

const Json& required(
    const Json& object,
    const char* key,
    const std::string_view source,
    const std::string& location) {
    if (!object.is_object()) {
        fail(source, location, "expected an object");
    }
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) {
        fail(source, location + "." + key, "missing required field");
    }
    return *found;
}

template <typename T>
T valueAs(
    const Json& value,
    const std::string_view source,
    const std::string& location,
    const char* expected) {
    try {
        return value.get<T>();
    } catch (const Json::exception&) {
        fail(source, location, std::string("expected ") + expected);
    }
}

float finiteFloat(
    const Json& value,
    const std::string_view source,
    const std::string& location) {
    const auto number = valueAs<float>(value, source, location, "a number");
    if (!std::isfinite(number)) {
        fail(source, location, "number must be finite");
    }
    return number;
}

StableId stableId(
    const Json& value,
    const std::string_view source,
    const std::string& location) {
    if (!value.is_number_unsigned() && !value.is_number_integer()) {
        fail(source, location, "expected a non-negative integer ID");
    }
    const auto signedValue = valueAs<std::int64_t>(
        value, source, location, "a non-negative integer ID");
    if (signedValue < 0) {
        fail(source, location, "ID must be non-negative");
    }
    return static_cast<StableId>(signedValue);
}

Vec2 readVec2(
    const Json& value,
    const std::string_view source,
    const std::string& location) {
    return {
        finiteFloat(required(value, "x", source, location), source, location + ".x"),
        finiteFloat(required(value, "y", source, location), source, location + ".y")
    };
}

Color readColor(
    const Json& value,
    const std::string_view source,
    const std::string& location) {
    return {
        finiteFloat(required(value, "x", source, location), source, location + ".x"),
        finiteFloat(required(value, "y", source, location), source, location + ".y"),
        finiteFloat(required(value, "z", source, location), source, location + ".z"),
        finiteFloat(required(value, "w", source, location), source, location + ".w")
    };
}

Json vec2Json(const Vec2 value) {
    return Json{{"x", value.x}, {"y", value.y}};
}

Json colorJson(const Color value) {
    return Json{{"x", value.r}, {"y", value.g}, {"z", value.b}, {"w", value.a}};
}

SpriteRenderer readSpriteRenderer(
    const Json& component,
    const ProjectPaths& paths,
    const std::string_view source,
    const std::string& location) {
    const auto& properties = required(component, "properties", source, location);
    const auto& spriteJson = required(properties, "sprite", source, location + ".properties");
    const auto& textureJson = required(spriteJson, "texture", source, location + ".properties.sprite");
    const auto& coordinates = required(
        spriteJson, "textureCoords", source, location + ".properties.sprite");
    if (!coordinates.is_array() || coordinates.size() != 4) {
        fail(source, location + ".properties.sprite.textureCoords", "expected exactly 4 coordinates");
    }

    SpriteRenderer renderer;
    renderer.id = stableId(
        required(properties, "UniqueId", source, location + ".properties"),
        source,
        location + ".properties.UniqueId");
    renderer.sprite.width = finiteFloat(
        required(spriteJson, "width", source, location + ".properties.sprite"),
        source,
        location + ".properties.sprite.width");
    renderer.sprite.height = finiteFloat(
        required(spriteJson, "height", source, location + ".properties.sprite"),
        source,
        location + ".properties.sprite.height");
    renderer.sprite.texture.pixelWidth = valueAs<int>(
        required(textureJson, "width", source, location + ".properties.sprite.texture"),
        source,
        location + ".properties.sprite.texture.width",
        "an integer");
    renderer.sprite.texture.pixelHeight = valueAs<int>(
        required(textureJson, "height", source, location + ".properties.sprite.texture"),
        source,
        location + ".properties.sprite.texture.height",
        "an integer");
    if (renderer.sprite.texture.pixelWidth <= 0 || renderer.sprite.texture.pixelHeight <= 0) {
        fail(source, location + ".properties.sprite.texture", "dimensions must be positive");
    }

    const auto assetId = textureJson.value("assetId", std::string{});
    const auto serializedPath = valueAs<std::string>(
        required(textureJson, "filepath", source, location + ".properties.sprite.texture"),
        source,
        location + ".properties.sprite.texture.filepath",
        "a string");
    renderer.sprite.texture.assetId =
        assetId.empty() && portableFilename(serializedPath) == "spritesheet.png"
        ? "spritesheet"
        : assetId;
    renderer.sprite.texture.resolvedPath =
        paths.resolveTexture(renderer.sprite.texture.assetId, serializedPath);

    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        renderer.sprite.textureCoordinates[index] = readVec2(
            coordinates[index],
            source,
            location + ".properties.sprite.textureCoords[" + std::to_string(index) + "]");
    }
    renderer.tint = readColor(
        required(properties, "color", source, location + ".properties"),
        source,
        location + ".properties.color");
    renderer.preservedComponent = component;
    return renderer;
}

GameObject readObject(
    const Json& objectJson,
    const ProjectPaths& paths,
    const std::string_view source,
    const std::size_t objectIndex,
    std::set<StableId>& objectIds,
    std::set<StableId>& componentIds) {
    const auto location = "objects[" + std::to_string(objectIndex) + "]";
    if (!objectJson.is_object()) {
        fail(source, location, "expected an object");
    }

    GameObject object;
    object.id = stableId(
        required(objectJson, "UniqueId", source, location),
        source,
        location + ".UniqueId");
    if (!objectIds.emplace(object.id).second) {
        fail(source, location + ".UniqueId", "duplicate object ID " + std::to_string(object.id));
    }
    object.name = valueAs<std::string>(
        required(objectJson, "name", source, location),
        source,
        location + ".name",
        "a string");
    object.zIndex = valueAs<int>(
        required(objectJson, "zIndex", source, location),
        source,
        location + ".zIndex",
        "an integer");

    const auto& transform = required(objectJson, "transform", source, location);
    object.transform.position = readVec2(
        required(transform, "position", source, location + ".transform"),
        source,
        location + ".transform.position");
    object.transform.scale = readVec2(
        required(transform, "scale", source, location + ".transform"),
        source,
        location + ".transform.scale");

    const auto& components = required(objectJson, "components", source, location);
    if (!components.is_array()) {
        fail(source, location + ".components", "expected an array");
    }
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto componentLocation =
            location + ".components[" + std::to_string(index) + "]";
        const auto& component = components[index];
        const auto type = valueAs<std::string>(
            required(component, "type", source, componentLocation),
            source,
            componentLocation + ".type",
            "a string");
        const auto& properties = required(component, "properties", source, componentLocation);
        const auto id = stableId(
            required(properties, "UniqueId", source, componentLocation + ".properties"),
            source,
            componentLocation + ".properties.UniqueId");
        if (!componentIds.emplace(id).second) {
            fail(source, componentLocation + ".properties.UniqueId",
                "duplicate component ID " + std::to_string(id));
        }

        if (type == SpriteRendererType) {
            object.components.emplace_back(
                readSpriteRenderer(component, paths, source, componentLocation));
        } else {
            object.components.emplace_back(UnknownComponent{id, type, component});
        }
    }

    object.preservedObject = objectJson;
    return object;
}

Json writeSpriteRenderer(const SpriteRenderer& renderer, const ProjectPaths& paths) {
    Json component = renderer.preservedComponent.is_object()
        ? renderer.preservedComponent
        : Json::object();
    component["type"] = SpriteRendererType;
    auto& properties = component["properties"];
    if (!properties.is_object()) {
        properties = Json::object();
    }
    properties["UniqueId"] = renderer.id;
    properties["color"] = colorJson(renderer.tint);

    auto& sprite = properties["sprite"];
    if (!sprite.is_object()) {
        sprite = Json::object();
    }
    sprite["width"] = renderer.sprite.width;
    sprite["height"] = renderer.sprite.height;

    auto& texture = sprite["texture"];
    if (!texture.is_object()) {
        texture = Json::object();
    }
    texture["assetId"] = renderer.sprite.texture.assetId;
    texture["filepath"] = paths.portableTexturePath(renderer.sprite.texture.resolvedPath);
    texture["width"] = renderer.sprite.texture.pixelWidth;
    texture["height"] = renderer.sprite.texture.pixelHeight;

    sprite["textureCoords"] = Json::array();
    for (const auto coordinate : renderer.sprite.textureCoordinates) {
        sprite["textureCoords"].push_back(vec2Json(coordinate));
    }
    return component;
}

Json writeObject(const GameObject& object, const ProjectPaths& paths) {
    Json output = object.preservedObject.is_object()
        ? object.preservedObject
        : Json::object();
    output["UniqueId"] = object.id;
    output["name"] = object.name;
    output["zIndex"] = object.zIndex;
    output["transform"] = Json{
        {"position", vec2Json(object.transform.position)},
        {"scale", vec2Json(object.transform.scale)}
    };
    output["components"] = Json::array();
    for (const auto& component : object.components) {
        std::visit(
            [&output, &paths](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, SpriteRenderer>) {
                    output["components"].push_back(writeSpriteRenderer(value, paths));
                } else {
                    output["components"].push_back(value.payload);
                }
            },
            component);
    }
    return output;
}

std::string operatingSystemError(const std::string& operation, const int error) {
    return operation + ": " + std::system_category().message(error);
}

class DefaultLevelFileOperations final : public LevelFileOperations {
public:
    std::filesystem::path temporaryPathFor(
        const std::filesystem::path& destination) override {
        const auto counter = TemporaryFileCounter.fetch_add(1, std::memory_order_relaxed);
        const auto timestamp = std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count();
#ifdef _WIN32
        const auto processId = static_cast<unsigned long long>(_getpid());
#else
        const auto processId = static_cast<unsigned long long>(getpid());
#endif
        auto filename = destination.filename().string();
        filename += ".tmp.";
        filename += std::to_string(processId);
        filename += ".";
        filename += std::to_string(timestamp);
        filename += ".";
        filename += std::to_string(counter);
        return destination.parent_path() / filename;
    }

    void writeDurably(
        const std::filesystem::path& temporaryFile,
        const std::string_view contents) override {
#ifdef _WIN32
        HANDLE file = CreateFileW(
            temporaryFile.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error(operatingSystemError(
                "Unable to create temporary level " + temporaryFile.string(),
                static_cast<int>(GetLastError())));
        }

        bool closed = false;
        try {
            std::size_t offset = 0;
            while (offset < contents.size()) {
                const auto remaining = contents.size() - offset;
                const auto request = static_cast<DWORD>(std::min<std::size_t>(
                    remaining,
                    std::numeric_limits<DWORD>::max()));
                DWORD written = 0;
                if (!WriteFile(
                        file,
                        contents.data() + offset,
                        request,
                        &written,
                        nullptr) ||
                    written == 0) {
                    throw std::runtime_error(operatingSystemError(
                        "Unable to write temporary level " + temporaryFile.string(),
                        static_cast<int>(GetLastError())));
                }
                offset += written;
            }
            if (!FlushFileBuffers(file)) {
                throw std::runtime_error(operatingSystemError(
                    "Unable to flush temporary level " + temporaryFile.string(),
                    static_cast<int>(GetLastError())));
            }
            if (!CloseHandle(file)) {
                file = INVALID_HANDLE_VALUE;
                throw std::runtime_error(operatingSystemError(
                    "Unable to close temporary level " + temporaryFile.string(),
                    static_cast<int>(GetLastError())));
            }
            closed = true;
        } catch (...) {
            if (!closed && file != INVALID_HANDLE_VALUE) {
                CloseHandle(file);
            }
            throw;
        }
#else
        const int file = open(
            temporaryFile.c_str(),
            O_WRONLY | O_CREAT | O_EXCL,
            0666);
        if (file < 0) {
            throw std::runtime_error(operatingSystemError(
                "Unable to create temporary level " + temporaryFile.string(),
                errno));
        }

        bool closed = false;
        try {
            std::size_t offset = 0;
            while (offset < contents.size()) {
                const auto result = write(
                    file,
                    contents.data() + offset,
                    contents.size() - offset);
                if (result < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw std::runtime_error(operatingSystemError(
                        "Unable to write temporary level " + temporaryFile.string(),
                        errno));
                }
                if (result == 0) {
                    throw std::runtime_error(
                        "Unable to write temporary level " + temporaryFile.string() +
                        ": zero-byte write");
                }
                offset += static_cast<std::size_t>(result);
            }
            if (fsync(file) != 0) {
                throw std::runtime_error(operatingSystemError(
                    "Unable to flush temporary level " + temporaryFile.string(),
                    errno));
            }
            if (close(file) != 0) {
                closed = true;
                throw std::runtime_error(operatingSystemError(
                    "Unable to close temporary level " + temporaryFile.string(),
                    errno));
            }
            closed = true;
        } catch (...) {
            if (!closed) {
                close(file);
            }
            throw;
        }
#endif
    }

    void replace(
        const std::filesystem::path& temporaryFile,
        const std::filesystem::path& destination) override {
#ifdef _WIN32
        if (!MoveFileExW(
                temporaryFile.c_str(),
                destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(operatingSystemError(
                "Unable to atomically replace level " + destination.string(),
                static_cast<int>(GetLastError())));
        }
#else
        if (rename(temporaryFile.c_str(), destination.c_str()) != 0) {
            throw std::runtime_error(operatingSystemError(
                "Unable to atomically replace level " + destination.string(),
                errno));
        }

        const auto parent = destination.parent_path().empty()
            ? std::filesystem::path{"."}
            : destination.parent_path();
#ifdef O_DIRECTORY
        const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
#else
        const int directory = open(parent.c_str(), O_RDONLY);
#endif
        if (directory < 0) {
            throw std::runtime_error(operatingSystemError(
                "Unable to open level directory for flush " + parent.string(),
                errno));
        }
        const int flushResult = fsync(directory);
        const int flushError = errno;
        const int closeResult = close(directory);
        if (flushResult != 0) {
            throw std::runtime_error(operatingSystemError(
                "Unable to flush level directory " + parent.string(),
                flushError));
        }
        if (closeResult != 0) {
            throw std::runtime_error(operatingSystemError(
                "Unable to close level directory " + parent.string(),
                errno));
        }
#endif
    }

    void removeTemporary(const std::filesystem::path& temporaryFile) noexcept override {
        std::error_code error;
        std::filesystem::remove(temporaryFile, error);
    }
};

} // namespace

World LevelIO::loadFile(const std::filesystem::path& file, const ProjectPaths& paths) {
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        throw LevelFormatError("Unable to open level file: " + file.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw LevelFormatError("Unable to read level file: " + file.string());
    }
    return loadString(contents.str(), paths, file.string());
}

World LevelIO::loadString(
    const std::string_view text,
    const ProjectPaths& paths,
    const std::string_view sourceName) {
    Json document;
    try {
        document = Json::parse(text);
    } catch (const Json::parse_error& error) {
        fail(sourceName, "JSON", error.what());
    }

    const Json* objects = nullptr;
    World world;
    if (document.is_array()) {
        objects = &document;
    } else if (document.is_object()) {
        const auto schema = document.find("schemaVersion");
        if (schema == document.end() || !schema->is_number_integer()) {
            fail(sourceName, "schemaVersion", "missing required integer field");
        }
        const auto version = schema->get<int>();
        if (version < 1 || version > CurrentSchemaVersion) {
            fail(sourceName, "schemaVersion", "unsupported version " + std::to_string(version));
        }
        objects = &required(document, "objects", sourceName, "document");
        world.documentMetadata = document;
        world.documentMetadata.erase("schemaVersion");
        world.documentMetadata.erase("objects");
    } else {
        fail(sourceName, "document", "expected a legacy root array or versioned object");
    }

    if (!objects->is_array()) {
        fail(sourceName, "objects", "expected an array");
    }

    std::set<StableId> objectIds;
    std::set<StableId> componentIds;
    std::vector<GameObject> loaded;
    loaded.reserve(objects->size());
    for (std::size_t index = 0; index < objects->size(); ++index) {
        loaded.emplace_back(
            readObject((*objects)[index], paths, sourceName, index, objectIds, componentIds));
    }
    world.replaceObjects(std::move(loaded));
    return world;
}

void LevelIO::saveFile(
    const std::filesystem::path& file,
    const World& world,
    const ProjectPaths& paths) {
    DefaultLevelFileOperations operations;
    saveFileWithOperations(file, world, paths, operations);
}

void LevelIO::saveFileWithOperations(
    const std::filesystem::path& file,
    const World& world,
    const ProjectPaths& paths,
    LevelFileOperations& operations) {
    const auto parent = file.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::runtime_error(
                "Unable to create level directory " + parent.string() + ": " + error.message());
        }
    }

    const auto temporaryFile = operations.temporaryPathFor(file);
    if (temporaryFile == file ||
        temporaryFile.parent_path() != file.parent_path()) {
        throw std::runtime_error(
            "Atomic level save requires a distinct sibling temporary file");
    }
    try {
        operations.writeDurably(temporaryFile, saveString(world, paths));
        operations.replace(temporaryFile, file);
    } catch (...) {
        operations.removeTemporary(temporaryFile);
        throw;
    }
}

std::string LevelIO::saveString(const World& world, const ProjectPaths& paths) {
    Json document = world.documentMetadata.is_object()
        ? world.documentMetadata
        : Json::object();
    document["schemaVersion"] = CurrentSchemaVersion;
    document["objects"] = Json::array();
    for (const auto& object : world.objects()) {
        document["objects"].push_back(writeObject(object, paths));
    }
    return document.dump(2) + "\n";
}

} // namespace mario::core
