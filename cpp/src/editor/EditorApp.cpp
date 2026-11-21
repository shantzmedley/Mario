#include "EditorApp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "mario/core/LevelIO.hpp"
#include "mario/core/ProjectPaths.hpp"
#include "mario/core/World.hpp"
#include "mario/editor/EditorPlayIntegration.hpp"

namespace mario::editor {
namespace {

using core::GameObject;
using core::StableId;
using core::Vec2;

constexpr float WorldWidth = 1280.0F;
constexpr float WorldHeight = 672.0F;
constexpr float GridSize = 32.0F;
constexpr float ViewAspect = 16.0F / 9.0F;
constexpr Vec2 CameraPosition{-250.0F, 0.0F};

[[noreturn]] void sdlFailure(const std::string& operation) {
    throw std::runtime_error(operation + ": " + SDL_GetError());
}

ImTextureID textureId(SDL_Texture* texture) {
    return static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(texture));
}

std::uint8_t colorByte(const float value) {
    return static_cast<std::uint8_t>(
        std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
}

float snapToGrid(const float value) {
    return std::floor(value / GridSize) * GridSize;
}

struct TextureDestroyer {
    void operator()(SDL_Texture* texture) const noexcept {
        if (texture != nullptr) {
            SDL_DestroyTexture(texture);
        }
    }
};

using TexturePtr = std::unique_ptr<SDL_Texture, TextureDestroyer>;

} // namespace

struct EditorApp::Impl {
    explicit Impl(
        std::filesystem::path projectRoot,
        std::unique_ptr<INetworkPlaySession> session,
        const EditorRunOptions options)
        : paths(std::move(projectRoot)),
          playIntegration(
              world,
              session ? std::move(session) : createUnavailableNetworkPlaySession()),
          runOptions(options) {}

    ~Impl() {
        shutdown();
    }

    int run() {
        try {
            initialize();
            loop();
            int exitCode = 0;
            if (levelLoaded && runOptions.persistLevelOnExit) {
                if (!save("Saved level on exit")) {
                    exitCode = 1;
                }
            }
            shutdown();
            return exitCode;
        } catch (const std::exception& error) {
            std::cerr << "Mario editor error: " << error.what() << '\n';
            shutdown();
            return 1;
        }
    }

    void initialize() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            sdlFailure("SDL_Init failed");
        }
        sdlInitialized = true;

        window = SDL_CreateWindow(
            "Mario C++ Editor",
            1600,
            900,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (window == nullptr) {
            sdlFailure("SDL_CreateWindow failed");
        }

        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) {
            sdlFailure("SDL_CreateRenderer failed");
        }
        SDL_SetRenderVSync(renderer, 1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imguiContextCreated = true;
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        if (runOptions.persistImGuiLayout) {
            char* preferencePath = SDL_GetPrefPath("Mario", "Editor");
            if (preferencePath == nullptr) {
                sdlFailure("SDL_GetPrefPath failed");
            }
            imguiIniPath =
                (std::filesystem::path{preferencePath} / "imgui.ini").string();
            SDL_free(preferencePath);
            io.IniFilename = imguiIniPath.c_str();
        } else {
            io.IniFilename = nullptr;
        }
        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
            throw std::runtime_error("ImGui SDL3 backend initialization failed");
        }
        imguiPlatformInitialized = true;
        if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
            throw std::runtime_error("ImGui SDL_Renderer3 backend initialization failed");
        }
        imguiRendererInitialized = true;

        spriteSheet = loadTexture(paths.spriteSheet());
        loadLevel();
        initialized = true;
    }

    TexturePtr loadTexture(const std::filesystem::path& file) {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(file.string().c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr) {
            throw std::runtime_error(
                "Unable to load image " + file.string() + ": " + stbi_failure_reason());
        }
        const auto freePixels = [](stbi_uc* data) { stbi_image_free(data); };
        std::unique_ptr<stbi_uc, decltype(freePixels)> ownedPixels(pixels, freePixels);

        TexturePtr texture{SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STATIC,
            width,
            height)};
        if (!texture) {
            sdlFailure("SDL_CreateTexture for spritesheet failed");
        }
        if (!SDL_UpdateTexture(texture.get(), nullptr, pixels, width * 4)) {
            sdlFailure("SDL_UpdateTexture for spritesheet failed");
        }
        SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST);
        spriteSheetWidth = width;
        spriteSheetHeight = height;
        return texture;
    }

    void loadLevel() {
        try {
            world = core::LevelIO::loadFile(paths.levelFile(), paths);
            levelLoaded = true;
            if (!world.objects().empty()) {
                selectedObject = world.objects().front().id;
            }
            status = "Loaded " + std::to_string(world.objects().size()) + " objects";
        } catch (const std::exception& error) {
            levelLoaded = false;
            status = std::string("Level load failed: ") + error.what();
        }
    }

    void loop() {
        bool done = false;
        int renderedFrames = 0;
        auto previousFrame = std::chrono::steady_clock::now();
        while (!done) {
            const auto currentFrame = std::chrono::steady_clock::now();
            const float deltaSeconds = std::clamp(
                std::chrono::duration<float>(currentFrame - previousFrame).count(),
                0.0F,
                0.1F);
            previousFrame = currentFrame;

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    done = true;
                }
            }

            pumpPlaySession(deltaSeconds);

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            handleEscape();
            renderDockspace();
            renderInspector();
            renderPalette();
            renderGameViewport();

            SDL_SetRenderTarget(renderer, nullptr);
            SDL_SetRenderDrawColor(renderer, 20, 22, 26, 255);
            SDL_RenderClear(renderer);
            ImGui::Render();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);

            ++renderedFrames;
            if (runOptions.frameLimit > 0 &&
                renderedFrames >= runOptions.frameLimit) {
                done = true;
            }
            if (quitRequested) {
                done = true;
            }
        }
    }

    void handleEscape() {
        if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            return;
        }
        if (playIntegration.running()) {
            playIntegration.stop();
            status = "Play stopped";
        } else {
            quitRequested = true;
        }
    }

    void pumpPlaySession(const float deltaSeconds) {
        playIntegration.pump(deltaSeconds);
        for (const auto& event : playIntegration.drainEvents()) {
            if (!event.message.empty()) {
                status = event.message;
            }
        }
    }

    void renderDockspace() {
        ImGuiWindowFlags flags =
            ImGuiWindowFlags_MenuBar |
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::Begin("Mario Editor Dockspace", nullptr, flags);
        ImGui::PopStyleVar(3);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::Button("Save")) {
                save("Saved level");
            }
            ImGui::SameLine();

            const bool canPlay =
                levelLoaded && playIntegration.available() && !playIntegration.running();
            ImGui::BeginDisabled(!canPlay);
            if (ImGui::Button("Play")) {
                std::string error;
                if (!playIntegration.startLocalHost(error)) {
                    status = error.empty() ? "Unable to start play session" : error;
                } else {
                    pendingTile.reset();
                    status = playIntegration.status();
                }
            }
            ImGui::EndDisabled();
            if (!canPlay && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", playIntegration.status().c_str());
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(!playIntegration.running());
            if (ImGui::Button("Stop")) {
                playIntegration.stop();
                status = playIntegration.status();
            }
            ImGui::EndDisabled();
            if (!playIntegration.running() &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", playIntegration.status().c_str());
            }

            ImGui::SameLine();
            ImGui::Separator();
            ImGui::SameLine();
            ImGui::TextUnformatted(status.c_str());
            ImGui::SameLine();
            const auto playState = playIntegration.state();
            const char* mode = playState.role == PlaySessionRole::Host
                ? "Local Host"
                : "Client";
            ImGui::TextDisabled("| %zu objects | %s", world.objects().size(), mode);
            if (playState.port != 0) {
                ImGui::SameLine();
                ImGui::TextDisabled(
                    "| %s:%u",
                    playState.endpoint.c_str(),
                    static_cast<unsigned int>(playState.port));
            }
            ImGui::SameLine();
            ImGui::TextDisabled(
                "| %s | connected:%s assigned:%s | tick:%llu",
                playState.status.c_str(),
                playState.connected ? "yes" : "no",
                playState.assigned ? "yes" : "no",
                static_cast<unsigned long long>(playState.tick));
            if (!playState.latestError.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(
                    ImVec4(1.0F, 0.35F, 0.30F, 1.0F),
                    "| %s",
                    playState.latestError.c_str());
            }
            ImGui::EndMenuBar();
        }

        ImGui::DockSpace(
            initializeDockLayout(),
            ImVec2(0.0F, 0.0F),
            ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();
    }

    ImGuiID initializeDockLayout() {
        const ImGuiID dockspaceId = ImGui::GetID("MarioEditorDockNode");
        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(
                dockspaceId,
                static_cast<ImGuiDockNodeFlags>(
                    static_cast<int>(ImGuiDockNodeFlags_DockSpace) |
                    static_cast<int>(ImGuiDockNodeFlags_PassthruCentralNode)));
            ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);

            ImGuiID center = dockspaceId;
            const ImGuiID palette = ImGui::DockBuilderSplitNode(
                center, ImGuiDir_Left, 0.22F, nullptr, &center);
            const ImGuiID inspector = ImGui::DockBuilderSplitNode(
                center, ImGuiDir_Right, 0.24F, nullptr, &center);
            ImGui::DockBuilderDockWindow("tsea", palette);
            ImGui::DockBuilderDockWindow("Inspector", inspector);
            ImGui::DockBuilderDockWindow("Game Viewport", center);
            ImGui::DockBuilderFinish(dockspaceId);
        }
        return dockspaceId;
    }

    void renderInspector() {
        ImGui::Begin("Inspector");
        GameObject* selected = selectedObject ? world.find(*selectedObject) : nullptr;
        if (selected == nullptr) {
            ImGui::TextDisabled("No object selected");
            ImGui::End();
            return;
        }

        ImGui::Text("Object %llu", static_cast<unsigned long long>(selected->id));
        ImGui::Text("Name: %s", selected->name.c_str());
        ImGui::Text(
            "Position: %.0f, %.0f",
            selected->transform.position.x,
            selected->transform.position.y);
        ImGui::Text("Z index: %d", selected->zIndex);

        if (auto* sprite = selected->spriteRenderer()) {
            float tint[4] = {
                sprite->tint.r, sprite->tint.g, sprite->tint.b, sprite->tint.a
            };
            if (ImGui::ColorEdit4("Sprite Tint", tint)) {
                sprite->tint = {tint[0], tint[1], tint[2], tint[3]};
                status = "Edited sprite tint";
            }
        } else {
            ImGui::TextDisabled("Selected object has no SpriteRenderer");
        }
        ImGui::End();
    }

    void renderPalette() {
        ImGui::Begin("tsea");
        ImGui::TextUnformatted("Spritesheet palette");
        ImGui::TextDisabled("Choose a tile, then click Game Viewport");
        ImGui::BeginDisabled(playIntegration.running());

        const float buttonSize = 32.0F;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float panelRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        for (std::size_t index = 0; index < 26; ++index) {
            const int column = static_cast<int>(index % 14);
            const int row = static_cast<int>(index / 14);
            const ImVec2 uv0{
                static_cast<float>(column * 16) / static_cast<float>(spriteSheetWidth),
                static_cast<float>(row * 16) / static_cast<float>(spriteSheetHeight)
            };
            const ImVec2 uv1{
                static_cast<float>((column + 1) * 16) / static_cast<float>(spriteSheetWidth),
                static_cast<float>((row + 1) * 16) / static_cast<float>(spriteSheetHeight)
            };

            ImGui::PushID(static_cast<int>(index));
            const bool isSelected = pendingTile == index;
            if (isSelected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25F, 0.65F, 0.30F, 1.0F));
            }
            const bool clicked = ImGui::ImageButton(
                    "tile",
                    textureId(spriteSheet.get()),
                    ImVec2(buttonSize, buttonSize),
                    uv0,
                    uv1);
            if (isSelected) {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
            if (clicked) {
                pendingTile = index;
                status = "Tile " + std::to_string(index) + " selected; click the viewport to place";
            }

            const float nextRight = ImGui::GetItemRectMax().x + spacing + buttonSize;
            if (index + 1 < 26 && nextRight < panelRight) {
                ImGui::SameLine();
            }
        }
        if (pendingTile.has_value()) {
            if (ImGui::Button("Cancel placement")) {
                pendingTile.reset();
                status = "Placement cancelled";
            }
        }
        ImGui::EndDisabled();
        ImGui::End();
    }

    void ensureViewportTarget(const int width, const int height) {
        if (viewportTarget && viewportWidth == width && viewportHeight == height) {
            return;
        }
        viewportTarget.reset();
        viewportTarget = TexturePtr{SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET,
            width,
            height)};
        if (!viewportTarget) {
            sdlFailure("SDL_CreateTexture for Game Viewport failed");
        }
        SDL_SetTextureScaleMode(viewportTarget.get(), SDL_SCALEMODE_NEAREST);
        viewportWidth = width;
        viewportHeight = height;
    }

    void renderWorld() {
        if (!SDL_SetRenderTarget(renderer, viewportTarget.get())) {
            sdlFailure("SDL_SetRenderTarget failed");
        }
        SDL_SetRenderDrawColor(renderer, 31, 35, 42, 255);
        SDL_RenderClear(renderer);

        const float scaleX = static_cast<float>(viewportWidth) / WorldWidth;
        const float scaleY = static_cast<float>(viewportHeight) / WorldHeight;

        SDL_SetRenderDrawColor(renderer, 61, 67, 78, 255);
        const float firstX = std::floor(CameraPosition.x / GridSize) * GridSize;
        const float lastX = CameraPosition.x + WorldWidth;
        for (float worldX = firstX; worldX <= lastX; worldX += GridSize) {
            const float screenX = (worldX - CameraPosition.x) * scaleX;
            SDL_RenderLine(renderer, screenX, 0.0F, screenX, static_cast<float>(viewportHeight));
        }
        const float firstY = std::floor(CameraPosition.y / GridSize) * GridSize;
        const float lastY = CameraPosition.y + WorldHeight;
        for (float worldY = firstY; worldY <= lastY; worldY += GridSize) {
            const float screenY =
                static_cast<float>(viewportHeight) - (worldY - CameraPosition.y) * scaleY;
            SDL_RenderLine(renderer, 0.0F, screenY, static_cast<float>(viewportWidth), screenY);
        }

        std::vector<const GameObject*> sorted;
        sorted.reserve(world.objects().size());
        for (const auto& object : world.objects()) {
            if (object.spriteRenderer() != nullptr) {
                sorted.push_back(&object);
            }
        }
        std::stable_sort(
            sorted.begin(),
            sorted.end(),
            [](const GameObject* left, const GameObject* right) {
                if (left->zIndex != right->zIndex) {
                    return left->zIndex < right->zIndex;
                }
                return left->id < right->id;
            });

        for (const auto* object : sorted) {
            drawObject(*object, scaleX, scaleY);
        }

        drawRuntimePlayers(scaleX, scaleY);

        if (pendingTile.has_value() && hoverWorld.has_value()) {
            GameObject preview;
            preview.transform.position = {
                snapToGrid(hoverWorld->x),
                snapToGrid(hoverWorld->y)
            };
            preview.transform.scale = {32.0F, 32.0F};
            core::SpriteRenderer sprite;
            sprite.sprite.textureCoordinates = core::spriteCoordinatesForTile(*pendingTile);
            sprite.tint.a = 0.55F;
            preview.components.emplace_back(sprite);
            drawObject(preview, scaleX, scaleY);
        }

        SDL_SetRenderTarget(renderer, nullptr);
    }

    void drawRuntimePlayers(const float scaleX, const float scaleY) {
        if (!playIntegration.running()) {
            return;
        }
        for (const auto& player : playIntegration.runtimePlayers()) {
            const SDL_FRect destination{
                (player.x - CameraPosition.x) * scaleX,
                static_cast<float>(viewportHeight) -
                    (player.y - CameraPosition.y + player.size) * scaleY,
                player.size * scaleX,
                player.size * scaleY
            };
            SDL_SetRenderDrawColor(
                renderer,
                player.red,
                player.green,
                player.blue,
                235);
            SDL_RenderFillRect(renderer, &destination);
            SDL_SetRenderDrawColor(
                renderer,
                player.local ? 255 : 30,
                player.local ? 255 : 30,
                player.local ? 255 : 30,
                255);
            SDL_RenderRect(renderer, &destination);
        }
    }

    void drawObject(const GameObject& object, const float scaleX, const float scaleY) {
        const auto* sprite = object.spriteRenderer();
        if (sprite == nullptr) {
            return;
        }

        float minU = std::numeric_limits<float>::max();
        float maxU = std::numeric_limits<float>::lowest();
        float minV = std::numeric_limits<float>::max();
        float maxV = std::numeric_limits<float>::lowest();
        for (const auto coordinate : sprite->sprite.textureCoordinates) {
            minU = std::min(minU, coordinate.x);
            maxU = std::max(maxU, coordinate.x);
            minV = std::min(minV, coordinate.y);
            maxV = std::max(maxV, coordinate.y);
        }

        const SDL_FRect source{
            minU * static_cast<float>(spriteSheetWidth),
            (1.0F - maxV) * static_cast<float>(spriteSheetHeight),
            (maxU - minU) * static_cast<float>(spriteSheetWidth),
            (maxV - minV) * static_cast<float>(spriteSheetHeight)
        };
        const SDL_FRect destination{
            (object.transform.position.x - CameraPosition.x) * scaleX,
            static_cast<float>(viewportHeight) -
                (object.transform.position.y - CameraPosition.y + object.transform.scale.y) * scaleY,
            object.transform.scale.x * scaleX,
            object.transform.scale.y * scaleY
        };

        SDL_SetTextureColorMod(
            spriteSheet.get(),
            colorByte(sprite->tint.r),
            colorByte(sprite->tint.g),
            colorByte(sprite->tint.b));
        SDL_SetTextureAlphaMod(spriteSheet.get(), colorByte(sprite->tint.a));
        SDL_RenderTexture(renderer, spriteSheet.get(), &source, &destination);

        if (selectedObject.has_value() && object.id == *selectedObject) {
            SDL_SetRenderDrawColor(renderer, 255, 210, 60, 255);
            SDL_RenderRect(renderer, &destination);
        }
    }

    void renderGameViewport() {
        ImGui::Begin(
            "Game Viewport",
            nullptr,
            ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoNavInputs);

        ImVec2 available = ImGui::GetContentRegionAvail();
        available.x = std::max(available.x, 1.0F);
        available.y = std::max(available.y, 1.0F);
        ImVec2 displaySize{available.x, available.x / ViewAspect};
        if (displaySize.y > available.y) {
            displaySize.y = available.y;
            displaySize.x = displaySize.y * ViewAspect;
        }

        const ImVec2 cursor = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2{
            cursor.x + (available.x - displaySize.x) * 0.5F,
            cursor.y + (available.y - displaySize.y) * 0.5F
        });

        ensureViewportTarget(
            std::max(1, static_cast<int>(std::round(displaySize.x))),
            std::max(1, static_cast<int>(std::round(displaySize.y))));
        renderWorld();
        ImGui::Image(textureId(viewportTarget.get()), displaySize);

        const bool hovered = ImGui::IsItemHovered();
        const bool focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (playIntegration.running()) {
            submitViewportInput(hovered, focused);
        }
        if (hovered) {
            const ImVec2 topLeft = ImGui::GetItemRectMin();
            const ImVec2 mouse = ImGui::GetMousePos();
            const float normalizedX =
                std::clamp((mouse.x - topLeft.x) / displaySize.x, 0.0F, 1.0F);
            const float normalizedY =
                std::clamp((mouse.y - topLeft.y) / displaySize.y, 0.0F, 1.0F);
            hoverWorld = Vec2{
                CameraPosition.x + normalizedX * WorldWidth,
                CameraPosition.y + (1.0F - normalizedY) * WorldHeight
            };

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (playIntegration.running()) {
                    status = "Editing is locked while Play is running";
                } else if (pendingTile.has_value() && levelLoaded) {
                    auto& placed = world.createSpriteObject(
                        *pendingTile,
                        {snapToGrid(hoverWorld->x), snapToGrid(hoverWorld->y)},
                        paths.spriteSheet());
                    selectedObject = placed.id;
                    status =
                        "Placed object " + std::to_string(placed.id) + " on the 32-unit grid";
                    pendingTile.reset();
                } else {
                    selectAt(*hoverWorld);
                }
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && pendingTile.has_value()) {
                pendingTile.reset();
                status = "Placement cancelled";
            }
        } else {
            hoverWorld.reset();
        }
        ImGui::End();
    }

    void submitViewportInput(const bool hovered, const bool focused) {
        ViewportInputState input;
        input.hovered = hovered;
        input.focused = focused;
        input.moveRight =
            ImGui::IsKeyDown(ImGuiKey_D) ||
            ImGui::IsKeyDown(ImGuiKey_RightArrow);
        input.moveLeft =
            ImGui::IsKeyDown(ImGuiKey_A) ||
            ImGui::IsKeyDown(ImGuiKey_LeftArrow);
        input.moveUp =
            ImGui::IsKeyDown(ImGuiKey_W) ||
            ImGui::IsKeyDown(ImGuiKey_UpArrow);
        input.moveDown =
            ImGui::IsKeyDown(ImGuiKey_S) ||
            ImGui::IsKeyDown(ImGuiKey_DownArrow);
        playIntegration.submitViewportInput(input);
    }

    void selectAt(const Vec2 worldPosition) {
        std::vector<const GameObject*> candidates;
        for (const auto& object : world.objects()) {
            if (object.spriteRenderer() == nullptr) {
                continue;
            }
            const auto& position = object.transform.position;
            const auto& scale = object.transform.scale;
            if (worldPosition.x >= position.x &&
                worldPosition.x <= position.x + scale.x &&
                worldPosition.y >= position.y &&
                worldPosition.y <= position.y + scale.y) {
                candidates.push_back(&object);
            }
        }
        if (candidates.empty()) {
            selectedObject.reset();
            status = "Selection cleared";
            return;
        }
        std::stable_sort(
            candidates.begin(),
            candidates.end(),
            [](const GameObject* left, const GameObject* right) {
                if (left->zIndex != right->zIndex) {
                    return left->zIndex > right->zIndex;
                }
                return left->id > right->id;
            });
        selectedObject = candidates.front()->id;
        status = "Selected object " + std::to_string(*selectedObject);
    }

    bool save(const std::string& successMessage) {
        if (!levelLoaded) {
            status = "Save disabled because the level did not load successfully";
            std::cerr << status << '\n';
            return false;
        }
        try {
            core::LevelIO::saveFile(paths.levelFile(), world, paths);
            status = successMessage;
            return true;
        } catch (const std::exception& error) {
            status = std::string("Save failed: ") + error.what();
            std::cerr << status << '\n';
            return false;
        }
    }

    void shutdown() noexcept {
        if (shutdownComplete) {
            return;
        }
        shutdownComplete = true;

        if (playIntegration.running()) {
            playIntegration.stop();
        }
        viewportTarget.reset();
        spriteSheet.reset();
        if (imguiRendererInitialized) {
            ImGui_ImplSDLRenderer3_Shutdown();
            imguiRendererInitialized = false;
        }
        if (imguiPlatformInitialized) {
            ImGui_ImplSDL3_Shutdown();
            imguiPlatformInitialized = false;
        }
        if (imguiContextCreated) {
            ImGui::DestroyContext();
            imguiContextCreated = false;
        }
        if (renderer != nullptr) {
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }
        if (window != nullptr) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        if (sdlInitialized) {
            SDL_Quit();
            sdlInitialized = false;
        }
    }

    core::ProjectPaths paths;
    core::World world;
    EditorPlayIntegration playIntegration;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    TexturePtr spriteSheet;
    TexturePtr viewportTarget;
    int spriteSheetWidth = 0;
    int spriteSheetHeight = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    EditorRunOptions runOptions;
    std::string imguiIniPath;
    std::optional<StableId> selectedObject;
    std::optional<std::size_t> pendingTile;
    std::optional<Vec2> hoverWorld;
    std::string status = "Initializing";
    bool levelLoaded = false;
    bool initialized = false;
    bool sdlInitialized = false;
    bool imguiContextCreated = false;
    bool imguiPlatformInitialized = false;
    bool imguiRendererInitialized = false;
    bool shutdownComplete = false;
    bool quitRequested = false;
};

EditorApp::EditorApp(
    std::filesystem::path projectRoot,
    std::unique_ptr<INetworkPlaySession> playSession,
    const EditorRunOptions options)
    : impl_(std::make_unique<Impl>(
          std::move(projectRoot),
          std::move(playSession),
          options)) {}

EditorApp::~EditorApp() = default;

int EditorApp::run() {
    return impl_->run();
}

} // namespace mario::editor
