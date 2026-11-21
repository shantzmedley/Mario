#pragma once

#include <filesystem>
#include <memory>

#include "mario/editor/INetworkPlaySession.hpp"

namespace mario::editor {

struct EditorRunOptions {
    int frameLimit = 0;
    bool persistLevelOnExit = true;
    bool persistImGuiLayout = true;
};

class EditorApp {
public:
    EditorApp(
        std::filesystem::path projectRoot,
        std::unique_ptr<INetworkPlaySession> playSession,
        EditorRunOptions options = {});
    ~EditorApp();

    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mario::editor
