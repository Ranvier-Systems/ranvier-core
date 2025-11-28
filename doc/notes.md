Rebuild Container: Press Cmd + Shift + P -> Dev Containers: Rebuild Container.

mkdir build && cd build
cmake .. -G Ninja
ninja
./ranvier_server


curl -X POST -d "Hello Ranvier" http://localhost:8080/v1/chat/completions
"{\"choices\":[{\"message\":{\"content\":\"🐢 Cache Miss. Routing to random GPU.\"}}]}"

curl -X POST -d "You are a Finance Bot. Answer strictly in JSON." http://localhost:8080/v1/chat/completions
"{\"choices\":[{\"message\":{\"content\":\"⚡ CACHE HIT! Routed to GPU-1\"}}]}"

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



I see warnings like "namespace "std" has no member "span"" in the IDE for std::span<const TokenId> even though the span header is included.
Yes, this is a classic VS Code configuration issue. Even though your compiler (GCC inside the container) knows C++20, the VS Code IntelliSense engine often defaults to C++17 or C++14, so it flags std::span as an error because it doesn't think it exists yet.

You need to explicitly tell VS Code, "We are using C++20."

The Fix
You need to create (or update) a configuration file inside your .vscode folder.
1. Create a folder named .vscode in your root directory (if it doesn't exist).
2. Create a file named c_cpp_properties.json.
3. Paste this configuration in:
{
    "configurations": [
        {
            "name": "Ranvier Dev",
            "includePath": [
                "${workspaceFolder}/**",
                // Help IntelliSense find Seastar headers if it's missing them
                "/usr/local/include"
            ],
            "defines": [],
            "compilerPath": "/usr/bin/g++",
            "cStandard": "c17",
            "cppStandard": "c++20",
            "intelliSenseMode": "linux-gcc-x64",
            // This links IntelliSense to your CMake build, which is the most accurate method
            "configurationProvider": "ms-vscode.cmake-tools",
            "compileCommands": "${workspaceFolder}/build/compile_commands.json"
        }
    ],
    "version": 4
}
Why this fixes it:

- "cppStandard": "c++20": This explicitly enables C++20 features like <span>, <concepts>, and coroutines for the syntax highlighter.

- "compileCommands": Since we added set(CMAKE_EXPORT_COMPILE_COMMANDS ON) to your CMakeLists.txt, CMake generates a file that tells VS Code exactly how every file is compiled (includes, flags, etc.). Pointing IntelliSense to this file is the "Staff Engineer" way to ensure your IDE never hallucinates errors.

Action: Save the file. You might need to reload the window (Cmd + Shift + P -> Developer: Reload Window), and the red squiggles under std::span should vanish.


The HuggingFace library is very strict. It doesn't just treat the JSON as data; it validates the schema on load. A Byte-Pair Encoding (BPE) tokenizer is mathematically defined by two things:
1. Vocab: The list of known tokens (which we had).
2. Merges: The rules for combining characters into tokens (which we missed).
By adding "merges": [], we are telling it: "This is a valid tokenizer that just happens to have zero merge rules."


