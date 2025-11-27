FROM fedora:39
# Install the heavy C++ tools + Seastar deps
RUN dnf install -y git cmake gcc-c++ seastar-devel ninja-build openssl-devel \
    && dnf clean all

# Optional: Install "nice to haves" like GDB, Valgrind, etc.
RUN dnf install -y gdb
