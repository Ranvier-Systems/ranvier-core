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


---
In a standard web server (like Go or Node.js), memory is shared. If you update a variable routes, all threads see it. In Seastar, memory is sharded. Your local_tree is thread_local. If you receive a POST /admin/routes on Core 0 and update the tree, Core 1 will not know about it. Requests hitting Core 1 will still miss the cache.

To implement a Control Plane, we must Broadcast the update to all CPU cores.
How broadcast_route Works
1. parallel_for_each: This spawns N futures (one for each core). It waits for all of them to complete.
2. smp::submit_to(shard, lambda): This is the "teleportation" function. It packages up the lambda and sends it to the message queue of the target CPU.
3. The Result: Even though RadixTree is thread_local (isolated), we have synchronized the state by replaying the command everywhere.

Test the Dynamic Router
1. Build and Run
ninja
./ranvier_server

2: Verify it knows nothing (Cache Miss)
curl -X POST -d "I need help with Python code." http://localhost:8080/v1/chat/completions
Result: 🐢 Cache Miss.

3: Teach it a new route (The "Coding Bot") We tell the router: "Any request starting with 'I need help with Python code' belongs on GPU-99."
curl -X POST "http://localhost:8080/admin/routes?backend_id=99" -d "I need help with Python code."
Result: {"status": "ok", "backend": 99}

4: Verify the new knowledge Send the completion request again.
curl -X POST -d "I need help with Python code. How do I sort a list?" http://localhost:8080/v1/chat/completions
Result: ⚡ CACHE HIT! Routed to GPU-99

The Distributed Control Plane runs inside a single process. If you check your server logs, you will see [Control Plane] Route added.... Crucially, because we used smp::submit_to, this route is now live on every single CPU core. No matter which thread handles the next request, it will find the route.

---
seastar::function_handler is a synchronous wrapper. It is trying to take your future and serialize the future object itself into JSON, rather than waiting for the future to complete. It was never designed for async workflows like broadcast_route.

We need to drop down to the "Raw" Seastar API for the Control Plane. We will use a custom helper struct (AsyncHandler) that handles the request/reply objects directly.
This simple wrapper allows you to write async lambdas for routes:
// Helper: Allows using async lambdas (futures) in routes
// function_handler is sync-only, so we need this for the Control Plane.
template <typename Func>
struct async_handler : public handler_base {
    Func _func;
    async_handler(Func&& f) : _func(std::move(f)) {}
    future<std::unique_ptr<reply>> handle(const sstring& path, std::unique_ptr<request> req, std::unique_ptr<reply> rep) override {
        return _func(std::move(req), std::move(rep));
    }
};

Replace your setup_routes function with this version.
* Data Plane: Keeps using function_handler (it's sync and fast).
* Control Plane: Swaps to async_handler.
Why this works
* std::unique_ptr<reply>: Instead of returning a string (which Seastar tries to auto-convert), we take ownership of the reply object.
* rep->write_body(...): We manually write the JSON string into the response buffer.
* move(rep): We carry the response object through the async chain (.then) and return it at the very end. This is the "Native" Seastar way to handle async I/O.
