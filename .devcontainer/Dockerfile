# Dev Container for Ranvier - Uses shared base image for environment parity
# Ensures dev environment exactly matches production build environment
#
# Option 1: Pull pre-built base from GitHub Container Registry (recommended)
#   docker pull ghcr.io/ranvier-systems/ranvier-base:latest
#
# Option 2: Build locally (if customizing toolchain)
#   docker build -f Dockerfile.base -t ghcr.io/ranvier-systems/ranvier-base:latest .
#
ARG BASE_IMAGE=ghcr.io/ranvier-systems/ranvier-base:latest
FROM ${BASE_IMAGE}

# Install only dev-specific tools not in the base image
RUN dnf install -y \
        gdb \
        strace \
        valgrind \
        zip \
        unzip \
    && pip3 install --no-cache-dir docker requests \
    && dnf clean all \
    && rm -rf /var/cache/dnf

WORKDIR /workspaces/ranvier-core
