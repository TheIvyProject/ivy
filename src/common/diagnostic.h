#pragma once

#include <cstdint>
#include <string>

namespace ivy {

struct Diagnostic {
    std::uint32_t line;  // 1-based
    std::uint32_t col;   // 1-based, in bytes
    std::string message;
};

}  // namespace ivy