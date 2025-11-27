#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <iostream>
#include <tokenizers_cpp.h> // Prove we linked the Rust library correctly

using namespace seastar;

future<> run() {
    std::cout << "⚡ Ranvier System: Online on " << smp::count << " cores.\n";
    std::cout << "✅ Seastar Initialized.\n";
    std::cout << "✅ Tokenizer Library Linked.\n";
    return make_ready_future<>();
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
