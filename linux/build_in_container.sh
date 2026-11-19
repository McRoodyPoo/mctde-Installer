#!/usr/bin/env bash
# Build the portable AppImage inside Valve's Steam Runtime "sniper" SDK
# container (Debian 11 base, glibc 2.31, the runtime Steam games use), so it
# runs on any current x86_64 distro: SteamOS, Ubuntu/Mint/Pop, Debian, Fedora,
# Arch and the rest. Called by make_appimage.sh; needs Docker or Podman.
#   Output: build/linux/mctde-Installer-x86_64.AppImage
# IMAGE overrides the container image.
set -euo pipefail
cd "$(dirname "$0")/.."

ENGINE=${ENGINE:-$(command -v podman || command -v docker || true)}
[[ -n $ENGINE ]] || {
    echo "Docker or Podman is needed for the portable build:"
    echo "  Arch:   sudo pacman -S --needed podman"
    echo "  Debian: sudo apt install podman"
    echo "(or MCTDE_NATIVE=1 ./make_appimage.sh for an AppImage that only runs on distros as new as this one)"
    exit 1
}
IMAGE=${IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest}

echo "Building the portable AppImage in $IMAGE with $(basename "$ENGINE")..."
# :Z relabels the mount for SELinux hosts (Fedora); a no-op elsewhere.
"$ENGINE" run --rm \
    -v "$PWD:/src:Z" -w /src \
    -e MCTDE_IN_CONTAINER=1 -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$IMAGE" bash linux/container_build.sh
