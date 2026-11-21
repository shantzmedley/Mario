# Mario Editor

The active editor foundation is a standalone C++20 application using SDL3,
SDL_Renderer3, and Dear ImGui docking. The Java/Gradle implementation remains
in `app/` as a legacy reference and is not required for the C++ build.

## Visual Studio 2022 WSL2 build

The WSL editor uses WSLg for its SDL window. Before configuring it for the
first time, install SDL3's Linux build dependencies and copy the current
sibling SyncVibe checkout into Visual Studio's Linux workspace:

```powershell
wsl.exe bash /mnt/c/Users/Shantz/Mario/scripts/setup-wsl.sh
```

This command prompts for the Ubuntu password because the development packages
are installed with `sudo`. Rerun it after changing SyncVibe so Visual Studio's
copied WSL source tree receives the latest runtime code.

In Visual Studio 2022:

1. Install the **C++ CMake tools for Linux and Mac** component.
2. Enable CMake Presets under **Tools > Options > CMake > General**.
3. Select **WSL2: Ubuntu** as the target system.
4. Select the `wsl2-debug` configure and build presets.
5. Select `mario_editor` as the Startup Item and press **F5**.

The checked-in preset copies Mario to `$HOME/.vs/Mario` and builds in the
Linux filesystem. The setup script places SyncVibe at the matching sibling
path `$HOME/.vs/SyncVibe`, which keeps network Play enabled. Do not hard-code
`DISPLAY`; Visual Studio inherits the WSLg display environment.

The same configuration can be validated directly from an Ubuntu shell:

```bash
cd /mnt/c/Users/Shantz/Mario
cmake --preset wsl2-debug
cmake --build --preset wsl2-debug
ctest --preset wsl2-debug
./out/build/wsl2-debug/mario_editor --smoke-test
```

## Network-enabled configure, build, test, and run

Dependencies are fetched from pinned Git revisions by CMake; no global package
manager or Ninja installation is required. Network Play defaults to ON when a
SyncVibe checkout exists beside Mario at `C:\Users\Shantz\SyncVibe`.

```powershell
cmake -S C:\Users\Shantz\Mario -B C:\Users\Shantz\Mario\build-cpp-network -G "Visual Studio 17 2022" -A x64 -DMARIO_ENABLE_NETWORK_PLAY=ON -DMARIO_SYNCVIBE_ROOT=C:\Users\Shantz\SyncVibe
cmake --build C:\Users\Shantz\Mario\build-cpp-network --config Debug --target mario_editor mario_core_tests mario_network_play_tests
ctest --test-dir C:\Users\Shantz\Mario\build-cpp-network -C Debug --output-on-failure
C:\Users\Shantz\Mario\build-cpp-network\Debug\mario_editor.exe
```

`MARIO_SYNCVIBE_ROOT` may point to any other SyncVibe checkout. In a fresh
standalone configure, Mario embeds that project in a private build directory
and supplies defaults that enable only the runtime. Explicit parent/CLI
SyncVibe cache choices are never overwritten. If a parent already provides
`SyncVibe::Runtime`, Mario links it without touching any SyncVibe cache option.

For a Release validation:

```powershell
cmake --build C:\Users\Shantz\Mario\build-cpp-network --config Release --target mario_editor mario_core_tests mario_network_play_tests
ctest --test-dir C:\Users\Shantz\Mario\build-cpp-network -C Release --output-on-failure
C:\Users\Shantz\Mario\build-cpp-network\Release\mario_editor.exe --project C:\Users\Shantz\Mario --network-smoke-test
```

The deterministic Mario-layer network smoke can also be run in Debug:

```powershell
C:\Users\Shantz\Mario\build-cpp-network\Debug\mario_editor.exe --project C:\Users\Shantz\Mario --network-smoke-test
```

It starts an ephemeral local server and client, waits for assignment, verifies
rightward authoritative movement, stops twice, and confirms runtime players
never enter serialized authoring data.

## Network-disabled and core-only builds

The editor remains supported without SyncVibe:

```powershell
cmake -S C:\Users\Shantz\Mario -B C:\Users\Shantz\Mario\build-cpp-offline -G "Visual Studio 17 2022" -A x64 -DMARIO_ENABLE_NETWORK_PLAY=OFF
cmake --build C:\Users\Shantz\Mario\build-cpp-offline --config Debug --target mario_editor mario_core_tests
ctest --test-dir C:\Users\Shantz\Mario\build-cpp-offline -C Debug --output-on-failure
```

To build only the core model and serializer tests without SDL, ImGui, stb, or
SyncVibe:

```powershell
cmake -S C:\Users\Shantz\Mario -B C:\Users\Shantz\Mario\build-cpp-core -G "Visual Studio 17 2022" -A x64 -DMARIO_BUILD_EDITOR=OFF -DMARIO_ENABLE_NETWORK_PLAY=OFF -DMARIO_BUILD_TESTS=ON
cmake --build C:\Users\Shantz\Mario\build-cpp-core --config Debug --target mario_core_tests
ctest --test-dir C:\Users\Shantz\Mario\build-cpp-core -C Debug --output-on-failure
```

Use `-DMARIO_BUILD_TESTS=OFF` when automated targets are unnecessary. Mario
uses its own source/binary directories rather than `CMAKE_SOURCE_DIR`, so it
can also be embedded with `add_subdirectory`.

## Level format migration

The reader accepts the original Java root-array `level.txt` format and
preserves object IDs, component IDs, and unknown component JSON. Legacy
absolute paths ending in `spritesheet.png` resolve to
`assets/images/spritesheet.png`.

New saves use a versioned wrapper:

```json
{
  "schemaVersion": 1,
  "objects": []
}
```

Known sprite textures are written with the portable asset ID `spritesheet` and
the relative path `assets/images/spritesheet.png`. The C++ reader accepts both
the legacy array and this wrapper. The legacy Java reader does not understand
the wrapper, so use the preserved Java code as reference rather than opening a
C++-saved level in the old editor.

## Editor behavior

The editor provides the `Inspector`, `tsea`, and `Game Viewport` panels. The
viewport is a resizable 16:9 render target with the original Y-up 1280x672
world projection and camera origin `(-250, 0)`. Choose one of the 26 palette
tiles, then click the viewport to place it on the 32-unit grid. Save is
available in the toolbar and also runs on normal exit. Saves are written and
flushed to a unique sibling file before an atomic same-directory replacement;
a pre-replace failure preserves the previous level. Save failures remain
visible, are logged to stderr, and make a normal editor exit return nonzero.
Dear ImGui layout state is stored in SDL's per-user preference directory, not
the repository. `--smoke-test` disables both level and ImGui-layout
persistence, so it is safe to run from the checkout root.

`Play` starts one local SyncVibe server on an ephemeral port and one loopback
client. The toolbar shows endpoint, connection/assignment state, authoritative
tick, status, and the latest error. `Stop` shuts down the client before the
server. While the Game Viewport is focused or hovered during Play:

- WASD and arrow keys submit normalized movement.
- Releasing movement keys submits zero input.
- Placement and selection are locked.
- Escape stops Play; pressing Escape again while stopped exits the editor.

Authoritative runtime players appear as colored 32x32 squares. The local
player is green with a white outline; remote players use stable ID-derived
colors. Runtime entities live only in `INetworkPlaySession` snapshots and are
never added to the authoring `World` or `level.txt`.

`PlaySessionController` owns the normalized network lifecycle.
`EditorPlayIntegration` binds that controller to the authoring `World`, input
policy, and runtime presentation proxies without inserting proxies into the
world. `SyncVibeNetworkPlaySession` is the only layer including
`<SyncVibe/Runtime.h>`; it pumps client/server/client on the editor thread and
maps runtime events and snapshots. Core world serialization has no SyncVibe
dependency.
