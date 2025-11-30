FROM fedora:39

# Install Basic Build Tools & Git
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
    && dnf clean all

# Install Python Deps
RUN dnf install -y python3-pip \
    && pip3 install docker requests

# Clone Seastar to get the dependency script
# We use a specific tag (master/latest) to ensure stability
WORKDIR /tmp
RUN git clone --depth 1 https://github.com/scylladb/seastar.git

# Install Seastar's System Dependencies
# This script automatically finds the right dnf packages for Fedora 39
RUN cd seastar && \
    ./install-dependencies.sh

# Build and Install Seastar (The Heavy Lifting)
# This takes ~15 mins but only runs ONCE when you build the image.
# We build in 'release' mode for performance.
RUN cd seastar && \
    ./configure.py --mode=release --prefix=/usr/local --c++-standard=20 && \
    ninja -C build/release install && \
    cd /tmp && rm -rf seastar

# Install Rust (Required for the Tokenizer library)
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
ENV PATH="/root/.cargo/bin:${PATH}"

# Cleanup
WORKDIR /workspaces/ranvier-core
