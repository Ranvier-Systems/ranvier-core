# Dev Container for Ranvier - Optimized for minimal disk usage
# All build steps consolidated into a single layer to minimize image size
FROM fedora:39

# Single consolidated layer: Install ALL dependencies, build Seastar, install Rust, cleanup
# This prevents intermediate artifacts from bloating image history
RUN dnf install -y \
        git \
        cmake \
        gcc-c++ \
        ninja-build \
        python3 \
        python3-pip \
        curl \
        zip \
        unzip \
        openssl-devel \
        libcurl-devel \
    # Clone and build Seastar in one go
    && cd /tmp \
    && git clone --depth 1 https://github.com/scylladb/seastar.git \
    && cd seastar \
    && ./install-dependencies.sh \
    && ./configure.py --mode=release --prefix=/usr/local --c++-standard=20 \
    && ninja -j$(nproc) -C build/release install \
    # Install Python packages
    && pip3 install --no-cache-dir docker requests \
    # Install Rust
    && curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y \
    # Cleanup everything in the same layer
    && cd /tmp \
    && rm -rf seastar \
    && dnf clean all \
    && rm -rf /var/cache/dnf \
    && rm -rf /root/.cache/pip \
    && rm -rf /tmp/*

ENV PATH="/root/.cargo/bin:${PATH}"

WORKDIR /workspaces/ranvier-core
