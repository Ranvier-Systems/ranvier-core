Rebuild Container: Press Cmd + Shift + P -> Dev Containers: Rebuild Container.

mkdir build && cd build
cmake .. -G Ninja
ninja
./ranvier_server


curl -X POST -d "Hello Ranvier" http://localhost:8080/v1/chat/completions

If the seastar build fails with:
ERROR: failed to build: failed to solve: ResourceExhausted
Increase Docker's memory limit:
By default, Docker doesn't access all your Mac's RAM.
1. Open Docker Desktop Dashboard.
2. Click the Gear Icon (Settings) in the top right.
3. Go to Resources → Resource Saver (or just "Resources" on older versions).
4. Look at Memory.
  - If you have a 16GB Mac, set Docker to 10GB.
  - If you have a 24GB/32GB Mac, set Docker to 16GB.
5. Swap: Set Swap to 2GB or 4GB (this acts as a safety net).
6. Click Apply & Restart.

Optionally, Throttle the build in the Dockerfile
- Edit .devcontainer/Dockerfile
- Find the RUN command where Seastar is built and change: ninja -C build/release install to ninja -j4 -C build/release install
Since the previous build left a "zombie" half-built layer, you need to force a clean build:
- Restart VS Code (to ensure the connection to Docker is reset).
- Open the Command Palette (Cmd + Shift + P).
- Select: Dev Containers: Rebuild Container Without Cache.
Note: Using -j4 will make the build take slightly longer, but it will actually finish without crashing
