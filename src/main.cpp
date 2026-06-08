// Ranvier Core - thin main() entry point for the ranvier_server binary.
//
// The server bring-up itself (CLI parsing, config loading, and the Seastar
// reactor run loop) lives in ranvier::run_application(), which is defined in
// run_application.cpp and compiled into the ranvier_core library. Keeping
// main() here as a one-line forwarder lets a downstream build link
// ranvier_core and supply its own main() — e.g. to wire up custom services
// before startup — without pulling in a second main().
#include "application.hpp"

int main(int argc, char** argv) {
    return ranvier::run_application(argc, argv);
}
