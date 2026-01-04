# Dev Container for Ranvier - Uses shared base image for environment parity
# Ensures dev environment exactly matches production build environment
#
# Build the base image first (once, or when Seastar/toolchain updates):
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
    && pip3 install --no-cache-dir docker requests \
    && dnf clean all \
    && rm -rf /var/cache/dnf

WORKDIR /workspaces/ranvier-core
