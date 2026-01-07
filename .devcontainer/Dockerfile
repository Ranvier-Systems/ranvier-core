# Dev Container for Ranvier - Uses shared base image for environment parity
# Ensures dev environment exactly matches production build environment
#
# Option 1: Pull pre-built base from GitHub Container Registry (after CI publishes it)
#   docker pull ghcr.io/ranvier-systems/ranvier-base:latest
#
# Option 2: Build locally (required until base image is published to ghcr.io)
#   docker build -f Dockerfile.base -t ranvier-dev/ranvier-base:latest .
#
ARG BASE_IMAGE=ranvier-dev/ranvier-base:latest
FROM ${BASE_IMAGE}

# Install only dev-specific tools not in the base image
RUN dnf install -y \
        gdb \
        strace \
        valgrind \
        zip \
        unzip \
        nmap-ncat \
    && pip3 install --no-cache-dir docker requests \
    && dnf clean all \
    && rm -rf /var/cache/dnf

WORKDIR /workspaces/ranvier-core
