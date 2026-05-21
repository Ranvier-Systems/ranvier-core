// libFuzzer harness for RequestRewriter::extract_*.
//
// Targets audit findings:
//   M6  — RapidJSON nesting limit / parse-error reporting on adversarial JSON
//   M4  — std::bad_alloc behaviour on large or pathological inputs
//   L5  — vocab id / max_token_id sign and bounds handling
//
// The fuzzer feeds the input as a JSON request body to each public
// extract_* entry point. We never assert anything about the result —
// libFuzzer will catch crashes, ASan will catch OOB / UAF, UBSan will
// catch signed overflow and other UB.
//
// See tests/fuzz/README.md for build and run instructions.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "chat_template.hpp"
#include "request_rewriter.hpp"

namespace {

void exercise_extractors(std::string_view body) {
    using ranvier::RequestRewriter;
    using ranvier::ChatTemplate;

    // Default chat template; we exercise the no-template path. Adding
    // template variants later widens coverage.
    ChatTemplate tmpl{};

    (void)RequestRewriter::extract_text_with_boundary_info(
        body, /*need_formatted_messages=*/true, tmpl);
    (void)RequestRewriter::extract_text_with_boundary_info(
        body, /*need_formatted_messages=*/false, tmpl);

    (void)RequestRewriter::extract_system_messages(body);

    (void)RequestRewriter::extract_prefix_token_count(body);
    (void)RequestRewriter::extract_prefix_boundaries(body);

    // Two max_token_id values: a normal upper bound and a small one that
    // makes most token ids invalid (drives the rejection path).
    (void)RequestRewriter::extract_prompt_token_ids(body, 200000);
    (void)RequestRewriter::extract_prompt_token_ids(body, 8);
    // Negative max_token_id is a config-validation edge — exercise it so
    // the comparison path can't sneak in a signed/unsigned bug.
    (void)RequestRewriter::extract_prompt_token_ids(body, -1);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view body(reinterpret_cast<const char*>(data), size);
    exercise_extractors(body);
    return 0;
}
