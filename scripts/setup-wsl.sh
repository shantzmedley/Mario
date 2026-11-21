#!/usr/bin/env bash

set -euo pipefail

if [[ "${MARIO_SKIP_APT:-0}" != "1" ]]; then
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
        build-essential \
        cmake \
        gdb \
        git \
        ninja-build \
        pkg-config \
        rsync \
        libasound2-dev \
        libpulse-dev \
        libfribidi-dev \
        libsndio-dev \
        libx11-dev \
        libxext-dev \
        libxrandr-dev \
        libxcursor-dev \
        libxfixes-dev \
        libxi-dev \
        libxss-dev \
        libxtst-dev \
        libxkbcommon-dev \
        libdrm-dev \
        libgbm-dev \
        libgl1-mesa-dev \
        libgles2-mesa-dev \
        libegl1-mesa-dev \
        libdbus-1-dev \
        libibus-1.0-dev \
        libudev-dev \
        libthai-dev \
        libusb-1.0-0-dev \
        libpipewire-0.3-dev \
        libwayland-dev \
        libdecor-0-dev \
        liburing-dev
fi

script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
mario_source="$(cd -- "${script_directory}/.." && pwd)"
syncvibe_source="${SYNCVIBE_SOURCE:-$(dirname -- "${mario_source}")/SyncVibe}"
visual_studio_root="${MARIO_VS_WSL_ROOT:-${HOME}/.vs}"
syncvibe_destination="${visual_studio_root}/SyncVibe"

if [[ ! -f "${syncvibe_source}/CMakeLists.txt" ]]; then
    printf 'SyncVibe was not found at %s\n' "${syncvibe_source}" >&2
    printf 'Set SYNCVIBE_SOURCE to its WSL-visible checkout and rerun this script.\n' >&2
    exit 1
fi

mkdir -p "${syncvibe_destination}"
rsync -a \
    --exclude '.git/' \
    --exclude '.vs/' \
    --exclude 'build/' \
    --exclude 'build-*/' \
    --exclude 'out/' \
    "${syncvibe_source}/" \
    "${syncvibe_destination}/"

printf 'WSL prerequisites are ready.\n'
printf 'SyncVibe copied to %s\n' "${syncvibe_destination}"
printf 'Select WSL2 and the wsl2-debug preset in Visual Studio 2022.\n'
