// Minimal Seastar stub for unit testing headers that include
// seastar/http/request.hh without requiring a full Seastar installation.
#pragma once

#include <string>

namespace seastar {
namespace http {

struct request {
    std::string content;
    std::string _method;
    std::string _url;
};

} // namespace http
} // namespace seastar
